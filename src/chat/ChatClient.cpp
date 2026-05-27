#include "ChatClient.h"

#include "../utils/HttpClient.h"
#include "../utils/Logger.h"

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>
#include <ixwebsocket/IXWebSocketMessageType.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <sstream>

namespace
{
// The chat base URL is accepted in any of four schemes: ws://, wss://,
// http://, https://. The user types whatever feels natural; we
// normalize internally to ws/wss for the WebSocket dial and http/
// https for the REST resolve / history endpoints.
std::string deriveHttpFromWs(const std::string &any)
{
    if (any.rfind("wss://",   0) == 0) return "https://" + any.substr(6);
    if (any.rfind("ws://",    0) == 0) return "http://"  + any.substr(5);
    if (any.rfind("https://", 0) == 0) return any;
    if (any.rfind("http://",  0) == 0) return any;
    return any;
}

std::string deriveWsFromHttp(const std::string &any)
{
    if (any.rfind("https://", 0) == 0) return "wss://" + any.substr(8);
    if (any.rfind("http://",  0) == 0) return "ws://"  + any.substr(7);
    if (any.rfind("wss://",   0) == 0) return any;
    if (any.rfind("ws://",    0) == 0) return any;
    return any;
}

// Whitelist-safe substitution for a streamId in a URL path. Stream IDs
// from the directory are already short hex, but a malformed value (from
// /meta on an old / hostile host) shouldn't be able to inject path
// segments or query separators into our REST URL.
std::string sanitizeStreamId(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (ok) out.push_back(c);
    }
    return out;
}
}

ChatClient::ChatClient() = default;

ChatClient::~ChatClient()
{
    disconnect();
}

void ChatClient::setBaseUrl(const std::string &baseUrl)
{
    std::string streamId;
    std::string nickname;
    bool        reconnect = false;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_baseUrl == baseUrl) return;
        m_baseUrl = baseUrl;
        streamId  = m_streamId;
        nickname  = m_nickname;
        reconnect = !streamId.empty() && m_state != State::Idle;
    }
    if (reconnect)
    {
        disconnect();
        connect(streamId, nickname);
    }
}

std::string ChatClient::getBaseUrl() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return m_baseUrl;
}

void ChatClient::connect(const std::string &streamId, const std::string &nickname)
{
    if (streamId.empty()) return;

    {
        std::lock_guard<std::mutex> lk(m_mu);
        const bool sameTarget = (m_streamId == streamId) &&
                                (m_nickname == nickname) &&
                                (m_state == State::Connected ||
                                 m_state == State::Connecting ||
                                 m_state == State::Resolving);
        if (sameTarget) return;
    }

    disconnect();

    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_streamId        = streamId;
        m_nickname        = nickname;
        m_messages.clear();
        m_participants.clear();
        m_myParticipantId.clear();
        m_roomId.clear();
        m_helloSent       = false;
        m_helloAcked      = false;
        m_unreadCount     = 0;
        m_lastError.clear();
        m_stopRequested.store(false);
        setStateLocked(State::Resolving);
    }

    m_resolverRunning.store(true);
    m_resolver = std::thread(&ChatClient::resolveAndConnect, this);
}

void ChatClient::disconnect()
{
    m_stopRequested.store(true);
    joinResolver();
    stopSession();
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_helloSent  = false;
        m_helloAcked = false;
        if (m_state != State::Error) setStateLocked(State::Idle);
    }
}

void ChatClient::joinResolver()
{
    if (m_resolver.joinable()) m_resolver.join();
    m_resolverRunning.store(false);
}

void ChatClient::stopSession()
{
    if (m_ws)
    {
        try { m_ws->stop(); } catch (...) {}
        m_ws.reset();
    }
}

void ChatClient::resolveAndConnect()
{
    std::string baseUrl;
    std::string streamId;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        baseUrl  = m_baseUrl;
        streamId = sanitizeStreamId(m_streamId);
    }

    if (baseUrl.empty() || streamId.empty())
    {
        std::lock_guard<std::mutex> lk(m_mu);
        setErrorLocked("missing baseUrl or streamId");
        return;
    }

    const std::string httpBase = deriveHttpFromWs(baseUrl);
    const std::string resolveUrl = httpBase + "/rooms/by-stream/" + streamId;

    auto resp = HttpClient::send(HttpClient::Method::GET, resolveUrl, "", 5000);
    if (m_stopRequested.load()) return;

    if (!resp.ok || resp.statusCode != 200)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        setErrorLocked("resolve failed: " +
                       (resp.error.empty() ? std::to_string(resp.statusCode) : resp.error));
        return;
    }

    std::string roomId;
    try
    {
        auto j = nlohmann::json::parse(resp.body);
        roomId = j.at("data").value("room_id", std::string{});
    }
    catch (const std::exception &e)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        setErrorLocked(std::string("resolve parse: ") + e.what());
        return;
    }
    if (roomId.empty())
    {
        std::lock_guard<std::mutex> lk(m_mu);
        setErrorLocked("resolve returned empty room_id");
        return;
    }

    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_roomId = roomId;
    }

    // Seed history before opening the WS so the panel can show
    // backlog the moment the user opens it. Failure here is
    // non-fatal — an empty history still renders fine.
    fetchHistory(roomId);
    if (m_stopRequested.load()) return;

    // Open the WS. IXWebSocket runs its own thread; we just register
    // the callback and call start().
    const std::string wsUrl = deriveWsFromHttp(baseUrl) + "/ws?room=" + roomId;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        setStateLocked(State::Connecting);
    }

    auto ws = std::make_unique<ix::WebSocket>();
    ws->setUrl(wsUrl);
    ws->disablePerMessageDeflate(); // matches server (USE_ZLIB=OFF)
    // Built-in retry: capped exponential backoff. 1 s..30 s mirrors
    // what the chat protocol doc says clients should do.
    ws->setMinWaitBetweenReconnectionRetries(1000);
    ws->setMaxWaitBetweenReconnectionRetries(30000);
    ws->enableAutomaticReconnection();
    ws->setHandshakeTimeout(5);

    ws->setOnMessageCallback(
        [this](const std::unique_ptr<ix::WebSocketMessage> &m)
        {
            if (m) onMessage(*m);
        });

    m_ws = std::move(ws);
    m_ws->start();
    // Past this point all I/O happens on the IX thread.
}

bool ChatClient::fetchHistory(const std::string &roomId)
{
    std::string baseUrl;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        baseUrl = m_baseUrl;
    }
    const std::string url = deriveHttpFromWs(baseUrl) +
                            "/rooms/" + roomId + "/history?limit=50";
    auto resp = HttpClient::send(HttpClient::Method::GET, url, "", 5000);
    if (!resp.ok || resp.statusCode != 200) return false;

    try
    {
        auto j = nlohmann::json::parse(resp.body);
        auto messages = j.at("data").at("messages");
        std::vector<Message> seed;
        seed.reserve(messages.size());
        for (const auto &m : messages)
        {
            Message out;
            out.id            = m.value("id", std::string{});
            out.participantId = m.value("participant_id", std::string{});
            out.nickname      = m.value("nickname", std::string{});
            out.body          = m.value("body", std::string{});
            out.postedAtMs    = m.value("posted_at_ms", int64_t{0});
            out.deleted       = m.value("deleted", false);
            seed.push_back(std::move(out));
        }
        // The history endpoint returns newest-first; we want oldest
        // first so the UI scrolls naturally.
        std::sort(seed.begin(), seed.end(),
                  [](const Message &a, const Message &b)
                  { return a.postedAtMs < b.postedAtMs; });

        std::lock_guard<std::mutex> lk(m_mu);
        for (auto &m : seed) m_messages.push_back(std::move(m));
        while (m_messages.size() > kMaxMessages) m_messages.pop_front();
        return true;
    }
    catch (const std::exception &e)
    {
        LOG_WARN(std::string("ChatClient: history parse failed: ") + e.what());
        return false;
    }
}

void ChatClient::onMessage(const ix::WebSocketMessage &msg)
{
    switch (msg.type)
    {
        case ix::WebSocketMessageType::Open:
        {
            // Send hello. If the server doesn't ack within HelloTimeout
            // it closes us — IXWebSocket reconnects automatically and
            // we re-send on the next Open.
            std::string nick;
            {
                std::lock_guard<std::mutex> lk(m_mu);
                nick         = m_nickname;
                m_helloSent  = false;
                m_helloAcked = false;
                setStateLocked(State::Connecting);
            }
            nlohmann::json hello = {
                {"kind",  "hello"},
                {"hello", { {"nickname", nick} }},
            };
            try
            {
                m_ws->sendText(hello.dump());
                std::lock_guard<std::mutex> lk(m_mu);
                m_helloSent = true;
            }
            catch (const std::exception &e)
            {
                LOG_WARN(std::string("ChatClient: hello send failed: ") + e.what());
            }
            break;
        }
        case ix::WebSocketMessageType::Message:
            handleFrame(msg.str);
            break;
        case ix::WebSocketMessageType::Close:
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_helloAcked = false;
            // If the user asked us to disconnect, stay Idle. Otherwise
            // IXWebSocket will reconnect — surface Reconnecting.
            if (!m_stopRequested.load() && m_state != State::Error)
            {
                setStateLocked(State::Reconnecting);
            }
            break;
        }
        case ix::WebSocketMessageType::Error:
        {
            std::lock_guard<std::mutex> lk(m_mu);
            // Transport-level errors keep the auto-reconnect loop
            // alive; we surface Reconnecting and stash the error so
            // the UI has something to show.
            m_lastError = msg.errorInfo.reason;
            if (!m_stopRequested.load() && m_state != State::Error)
            {
                setStateLocked(State::Reconnecting);
            }
            break;
        }
        default:
            break;
    }
}

void ChatClient::handleFrame(const std::string &payload)
{
    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(payload);
    }
    catch (const std::exception &e)
    {
        LOG_WARN(std::string("ChatClient: bad WS frame: ") + e.what());
        return;
    }

    const std::string kind = j.value("kind", std::string{});

    if (kind == "welcome")
    {
        const auto w = j.value("welcome", nlohmann::json::object());
        std::lock_guard<std::mutex> lk(m_mu);
        m_myParticipantId = w.value("participant_id", std::string{});
        m_helloAcked      = true;
        setStateLocked(State::Connected);
        m_lastError.clear();
    }
    else if (kind == "room_state")
    {
        const auto rs   = j.value("room_state", nlohmann::json::object());
        const auto plist = rs.value("participants", nlohmann::json::array());
        std::vector<Participant> ps;
        ps.reserve(plist.size());
        for (const auto &p : plist)
        {
            Participant out;
            out.id       = p.value("participant_id", std::string{});
            out.nickname = p.value("nickname", std::string{});
            ps.push_back(std::move(out));
        }
        std::lock_guard<std::mutex> lk(m_mu);
        if (ps.size() > kMaxParticipants) ps.resize(kMaxParticipants);
        m_participants = std::move(ps);
    }
    else if (kind == "message")
    {
        const auto m = j.value("message", nlohmann::json::object());
        Message out;
        out.id            = m.value("id", std::string{});
        out.participantId = m.value("participant_id", std::string{});
        out.nickname      = m.value("nickname", std::string{});
        out.body          = m.value("body", std::string{});
        out.postedAtMs    = m.value("posted_at_ms", int64_t{0});
        out.deleted       = m.value("deleted", false);
        appendMessage(std::move(out));
    }
    else if (kind == "deleted")
    {
        const auto d  = j.value("deleted", nlohmann::json::object());
        const std::string mid = d.value("id", std::string{});
        std::lock_guard<std::mutex> lk(m_mu);
        for (auto &m : m_messages)
        {
            if (m.id == mid)
            {
                m.deleted = true;
                m.body    = "[message removed]";
                break;
            }
        }
    }
    else if (kind == "presence")
    {
        const auto p   = j.value("presence", nlohmann::json::object());
        const std::string event = p.value("event", std::string{});
        const std::string pid   = p.value("participant_id", std::string{});
        const std::string nick  = p.value("nickname", std::string{});
        std::lock_guard<std::mutex> lk(m_mu);
        if (event == "join")
        {
            auto it = std::find_if(m_participants.begin(), m_participants.end(),
                                   [&](const Participant &x) { return x.id == pid; });
            if (it == m_participants.end() && m_participants.size() < kMaxParticipants)
            {
                m_participants.push_back({pid, nick});
            }
        }
        else if (event == "leave")
        {
            m_participants.erase(
                std::remove_if(m_participants.begin(), m_participants.end(),
                               [&](const Participant &x) { return x.id == pid; }),
                m_participants.end());
        }
    }
    else if (kind == "error")
    {
        const auto e = j.value("error", nlohmann::json::object());
        const std::string code = e.value("code",    std::string{});
        const std::string text = e.value("message", std::string{});
        std::lock_guard<std::mutex> lk(m_mu);
        m_lastError = code.empty() ? text : (code + ": " + text);
        // Server-side errors don't kill the connection unless it
        // followed up with a close; just surface for the UI.
    }
    // pong is informational; ignore.
}

void ChatClient::appendMessage(Message m)
{
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_myParticipantId.empty() && m.participantId == m_myParticipantId)
    {
        m.local = true;
    }
    m_messages.push_back(std::move(m));
    while (m_messages.size() > kMaxMessages) m_messages.pop_front();
    m_unreadCount += 1;
}

void ChatClient::post(const std::string &body)
{
    if (body.empty()) return;

    bool canSend = false;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        canSend = (m_state == State::Connected) && m_ws != nullptr && m_helloAcked;
    }
    if (!canSend) return;

    nlohmann::json frame = {
        {"kind", "post"},
        {"post", { {"body", body} }},
    };
    try
    {
        m_ws->sendText(frame.dump());
    }
    catch (const std::exception &e)
    {
        LOG_WARN(std::string("ChatClient: post failed: ") + e.what());
    }
}

void ChatClient::setNickname(const std::string &nick)
{
    std::string streamId;
    std::string currentNick;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_nickname == nick) return;
        m_nickname  = nick;
        streamId    = m_streamId;
        currentNick = nick;
    }
    // Reconnect to re-handshake — the wire protocol doesn't have a
    // mid-session rename frame.
    if (!streamId.empty())
    {
        disconnect();
        connect(streamId, currentNick);
    }
}

ChatClient::Snapshot ChatClient::getSnapshot() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    Snapshot s;
    s.state           = m_state;
    s.lastError       = m_lastError;
    s.baseUrl         = m_baseUrl;
    s.streamId        = m_streamId;
    s.roomId          = m_roomId;
    s.nickname        = m_nickname;
    s.myParticipantId = m_myParticipantId;
    s.messages.assign(m_messages.begin(), m_messages.end());
    s.participants    = m_participants;
    s.unreadCount     = m_unreadCount;
    return s;
}

void ChatClient::markRead()
{
    std::lock_guard<std::mutex> lk(m_mu);
    m_unreadCount = 0;
}

bool ChatClient::isActive() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return m_state != State::Idle && m_state != State::Error;
}

void ChatClient::setStateLocked(State s)
{
    m_state = s;
}

void ChatClient::setErrorLocked(const std::string &err)
{
    m_lastError = err;
    m_state     = State::Error;
    LOG_WARN("ChatClient: " + err);
}

std::string ChatClient::httpBase() const
{
    std::lock_guard<std::mutex> lk(m_mu);
    return deriveHttpFromWs(m_baseUrl);
}
