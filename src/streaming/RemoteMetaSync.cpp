#include "RemoteMetaSync.h"
#include "../utils/Logger.h"

#include <nlohmann/json.hpp>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socket_t = SOCKET;
  static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
  static int closeSocket(socket_t s) { return ::closesocket(s); }
#else
  #include <sys/socket.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using socket_t = int;
  static constexpr socket_t INVALID_SOCK = -1;
  static int closeSocket(socket_t s) { return ::close(s); }
#endif

namespace
{
    struct UrlParts
    {
        std::string host;
        std::string port;
        std::string path;
    };

    // Minimal http:// parser — strict enough for /meta polling, not a
    // general-purpose URL library. Recognises "http://<host>[:<port>][<path>]".
    bool parseHttpUrl(const std::string &url, UrlParts &out)
    {
        constexpr const char *prefix = "http://";
        if (url.compare(0, 7, prefix) != 0) return false;

        std::string rest = url.substr(7);
        size_t pathStart = rest.find('/');
        std::string hostPort = (pathStart == std::string::npos) ? rest : rest.substr(0, pathStart);
        out.path = (pathStart == std::string::npos) ? "/" : rest.substr(pathStart);

        size_t colon = hostPort.find(':');
        if (colon == std::string::npos)
        {
            out.host = hostPort;
            out.port = "80";
        }
        else
        {
            out.host = hostPort.substr(0, colon);
            out.port = hostPort.substr(colon + 1);
        }
        return !out.host.empty();
    }

    // Opens a TCP connection to host:port. Returns INVALID_SOCK on failure.
    socket_t connectTcp(const UrlParts &u)
    {
        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *res = nullptr;
        if (getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res) != 0 || !res)
        {
            return INVALID_SOCK;
        }
        socket_t sock = INVALID_SOCK;
        for (addrinfo *p = res; p != nullptr; p = p->ai_next)
        {
            sock = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (sock == INVALID_SOCK) continue;
            if (::connect(sock, p->ai_addr, static_cast<int>(p->ai_addrlen)) == 0) break;
            closeSocket(sock);
            sock = INVALID_SOCK;
        }
        freeaddrinfo(res);
        return sock;
    }

    // Synchronous HTTP/1.1 GET with a 5-second receive deadline. Returns
    // the response body on 2xx, empty string otherwise. Logs errors but
    // doesn't throw — callers treat empty as "skip this poll".
    //
    // authToken (#49 Phase 3) is the sha256 hex of the user's stream
    // password; when non-empty it's sent as an Authorization: Bearer
    // header. Empty token means "no auth", header omitted.
    std::string httpGet(const std::string &fullUrl, const std::string &authToken = "")
    {
        UrlParts u;
        if (!parseHttpUrl(fullUrl, u))
        {
            LOG_WARN("RemoteMetaSync: malformed URL: " + fullUrl);
            return {};
        }

        socket_t sock = connectTcp(u);
        if (sock == INVALID_SOCK) return {};

        // 5-second receive timeout so a wedged remote doesn't stall the
        // polling thread forever.
#ifdef _WIN32
        DWORD tv = 5000;
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tv), sizeof(tv));
#else
        timeval tv{};
        tv.tv_sec = 5;
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        std::string req;
        req.reserve(256);
        req += "GET ";
        req += u.path;
        req += " HTTP/1.1\r\nHost: ";
        req += u.host;
        if (u.port != "80")
        {
            req += ':';
            req += u.port;
        }
        req += "\r\nConnection: close\r\nAccept: application/json\r\n";
        if (!authToken.empty())
        {
            req += "Authorization: Bearer ";
            req += authToken;
            req += "\r\n";
        }
        req += "\r\n";

        if (::send(sock, req.data(), static_cast<int>(req.size()), 0) < 0)
        {
            closeSocket(sock);
            return {};
        }

        std::string response;
        response.reserve(4096);
        char buf[2048];
        for (;;)
        {
            int n = ::recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) break;
            response.append(buf, static_cast<size_t>(n));
            if (response.size() > 256 * 1024) break; // sanity cap
        }
        closeSocket(sock);

        // Header / body split.
        size_t sep = response.find("\r\n\r\n");
        if (sep == std::string::npos) return {};

        // Reject non-2xx.
        if (response.size() < 12) return {};
        // "HTTP/1.1 200 ..."
        const std::string statusLine = response.substr(0, response.find("\r\n"));
        size_t firstSpace = statusLine.find(' ');
        if (firstSpace == std::string::npos) return {};
        int statusCode = std::atoi(statusLine.c_str() + firstSpace + 1);
        if (statusCode < 200 || statusCode >= 300) return {};

        return response.substr(sep + 4);
    }

    // FFmpeg-avio-backed HTTP GET. Handles http:// and https:// equally
    // — the TLS stack FFmpeg was linked against (OpenSSL / GnuTLS /
    // SChannel) does the heavy lifting, so we don't carry our own
    // crypto. Used by the short-poll path (fetchOnce); the raw-socket
    // httpGet above is kept around for the SSE handshake which
    // doesn't need TLS yet (the SSE path is plaintext-only and falls
    // back to short-poll automatically when the URL is https://).
    //
    // Returns the response body on 2xx; empty string on any failure.
    // FFmpeg's avio surfaces HTTP errors as negative avio_read results
    // and we treat those as "skip this poll" — matching the original
    // httpGet's contract.
    std::string httpGetViaAvio(const std::string &fullUrl,
                                const std::string &authToken,
                                int recvTimeoutMs = 5000)
    {
        avformat_network_init();

        AVDictionary *opts = nullptr;
        av_dict_set_int(&opts, "rw_timeout", static_cast<int64_t>(recvTimeoutMs) * 1000, 0); // µs
        av_dict_set(&opts, "user_agent", "RetroCapture/0.7", 0);
        std::string headers = "Accept: application/json\r\n";
        if (!authToken.empty())
        {
            headers += "Authorization: Bearer ";
            headers += authToken;
            headers += "\r\n";
        }
        av_dict_set(&opts, "headers", headers.c_str(), 0);

        AVIOContext *io = nullptr;
        int rc = avio_open2(&io, fullUrl.c_str(), AVIO_FLAG_READ, nullptr, &opts);
        av_dict_free(&opts);
        if (rc < 0)
        {
            char errbuf[256] = {};
            av_strerror(rc, errbuf, sizeof(errbuf));
            LOG_WARN(std::string("RemoteMetaSync: avio_open2 failed for ") + fullUrl + ": " + errbuf);
            return {};
        }

        std::string body;
        body.reserve(4096);
        unsigned char buf[2048];
        for (;;)
        {
            int n = avio_read(io, buf, sizeof(buf));
            if (n <= 0) break;
            body.append(reinterpret_cast<const char *>(buf), static_cast<size_t>(n));
            if (body.size() > 256 * 1024) break; // sanity cap
        }
        avio_closep(&io);
        return body;
    }

    // Parses the /meta JSON snapshot body into the Snapshot struct.
    // Returns false on parse error. Shared by both the short-poll
    // (httpGet) and long-poll (SSE) paths.
    bool parseSnapshotJSON(const std::string &body, RemoteMetaSync::Snapshot &out)
    {
        try
        {
            auto j = nlohmann::json::parse(body);
            out.protocolVersion = j.value("protocolVersion", 0);
            out.serverVersion   = j.value("serverVersion", std::string{});

            if (j.contains("shader") && j["shader"].is_object())
            {
                const auto &s = j["shader"];
                out.shaderActive    = s.value("active", false);
                out.pipelineEnabled = s.value("pipelineEnabled", true);
                out.preset          = s.value("preset", std::string{});
                out.presetHash      = s.value("presetHash", std::string{});
                if (s.contains("parameters") && s["parameters"].is_array())
                {
                    out.parameters.clear();
                    out.parameters.reserve(s["parameters"].size());
                    for (const auto &p : s["parameters"])
                    {
                        RemoteMetaSync::ParamOverride po;
                        po.name  = p.value("name", std::string{});
                        po.value = p.value("value", 0.0f);
                        if (!po.name.empty())
                        {
                            out.parameters.push_back(std::move(po));
                        }
                    }
                }
            }

            if (j.contains("source") && j["source"].is_object())
            {
                const auto &s = j["source"];
                out.sourceWidth  = s.value("width",  0u);
                out.sourceHeight = s.value("height", 0u);
                out.sourceFps    = s.value("fps",    0u);
            }

            if (j.contains("image") && j["image"].is_object())
            {
                const auto &im = j["image"];
                out.imageBrightness     = im.value("brightness", 1.0f);
                out.imageContrast       = im.value("contrast",   1.0f);
                out.imageMaintainAspect = im.value("maintainAspect", true);
                out.imageOutputWidth    = im.value("outputWidth",  0u);
                out.imageOutputHeight   = im.value("outputHeight", 0u);
            }
            if (j.contains("streaming") && j["streaming"].is_object())
            {
                // Only field we need from the streaming block in client
                // mode is the host's current viewer count (#68 — fed to
                // the OSD quick-actions widget). Active / url describe
                // the host's local state which the client doesn't
                // surface anywhere.
                const auto &st = j["streaming"];
                out.upstreamClientCount = st.value("clientCount", 0u);
            }
            return true;
        }
        catch (const std::exception &e)
        {
            LOG_WARN(std::string("RemoteMetaSync: JSON parse failed — ") + e.what());
            return false;
        }
    }
}

RemoteMetaSync::RemoteMetaSync() = default;

RemoteMetaSync::~RemoteMetaSync()
{
    stop();
}

bool RemoteMetaSync::start(const std::string &baseUrl, SnapshotCallback cb, int pollIntervalMs)
{
    if (m_running.load())
    {
        LOG_WARN("RemoteMetaSync::start — already running");
        return false;
    }
    if (baseUrl.empty() || !cb)
    {
        return false;
    }
    m_baseUrl = baseUrl;
    if (!m_baseUrl.empty() && m_baseUrl.back() == '/')
    {
        m_baseUrl.pop_back();
    }
    m_cb = std::move(cb);
    m_pollIntervalMs = std::max(250, pollIntervalMs);

    m_lastPreset.clear();
    m_lastPresetHash.clear();
    m_lastPipelineEnabled = true;
    m_lastParamValues.clear();

    m_running.store(true);
    m_thread = std::thread(&RemoteMetaSync::pollLoop, this);
    LOG_INFO("RemoteMetaSync: polling " + m_baseUrl + "/meta every " +
             std::to_string(m_pollIntervalMs) + " ms");
    return true;
}

void RemoteMetaSync::stop()
{
    if (!m_running.load()) return;
    m_running.store(false);
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

bool RemoteMetaSync::fetchOnce(Snapshot &out)
{
    // Use FFmpeg's avio so http:// and https:// both work transparently.
    // The host's tunnel URL (Cloudflare Quick Tunnel etc.) is HTTPS,
    // and our hand-rolled httpGet only speaks plaintext — using avio
    // here piggybacks on the TLS stack FFmpeg is already linked
    // against (OpenSSL / GnuTLS / SChannel depending on the build).
    std::string body = httpGetViaAvio(m_baseUrl + "/meta", m_authToken);
    if (body.empty()) return false;
    return parseSnapshotJSON(body, out);
}

void RemoteMetaSync::dispatchIfChanged(const Snapshot &snap)
{
    std::vector<float> currentValues;
    currentValues.reserve(snap.parameters.size());
    for (const auto &p : snap.parameters)
    {
        currentValues.push_back(p.value);
    }

    const bool presetChanged  = (snap.preset           != m_lastPreset);
    const bool hashChanged    = (snap.presetHash       != m_lastPresetHash);
    const bool toggleChanged  = (snap.pipelineEnabled  != m_lastPipelineEnabled);
    const bool paramsChanged  = (currentValues         != m_lastParamValues);
    // Source resolution/fps must participate in the delta check, otherwise
    // a host that switches capture resolution (e.g. 1920x1080 → 1280x720)
    // never wakes the client up — the new dimensions live in every /meta
    // payload but match preset/hash/params from before, so the snapshot
    // gets dropped here and the client keeps allocating textures at the
    // old size. Treat any change in source dims/fps as a delta worth
    // dispatching so applyPendingRemoteMeta() can resize the capture.
    const bool sourceChanged  = (snap.sourceWidth      != m_lastSourceWidth)  ||
                                (snap.sourceHeight     != m_lastSourceHeight) ||
                                (snap.sourceFps        != m_lastSourceFps);
    // #68 — upstream client count also participates in the delta
    // check. Without this, a viewer joining or leaving the host's
    // stream never wakes the callback up (preset/hash/source all
    // unchanged), so the OSD quick-actions widget stays frozen at
    // whatever count was active on first connect.
    const bool viewersChanged = (snap.upstreamClientCount != m_lastUpstreamClientCount);

    if (presetChanged || hashChanged || toggleChanged || paramsChanged || sourceChanged || viewersChanged)
    {
        if (m_cb) m_cb(snap);
        m_lastPreset                = snap.preset;
        m_lastPresetHash            = snap.presetHash;
        m_lastPipelineEnabled       = snap.pipelineEnabled;
        m_lastParamValues           = std::move(currentValues);
        m_lastSourceWidth           = snap.sourceWidth;
        m_lastSourceHeight          = snap.sourceHeight;
        m_lastSourceFps             = snap.sourceFps;
        m_lastUpstreamClientCount   = snap.upstreamClientCount;
    }
}

// Long-poll on /meta with SSE. Phase 6 of #47.
//
// Returns true if the SSE handshake succeeded and the connection then
// ended (server closed, network blip, stop requested). Returns false if
// the server's response wasn't text/event-stream — caller falls back to
// short-poll fetchOnce(). Old RetroCapture builds (Phases 1-5) that only
// know HTTP GET on /meta will land in the false branch and the client
// transparently degrades to 1 s polling.
bool RemoteMetaSync::runSSE()
{
    UrlParts u;
    if (!parseHttpUrl(m_baseUrl + "/meta", u))
    {
        return false;
    }

    socket_t sock = connectTcp(u);
    if (sock == INVALID_SOCK) return false;

    // No recv timeout for SSE — the connection is intentionally long-lived.
    // We rely on send-failure detection on the server side (their keepalive
    // comment + payload writes) to notice broken sockets.

    std::string req;
    req.reserve(256);
    req += "GET ";
    req += u.path;
    req += " HTTP/1.1\r\nHost: ";
    req += u.host;
    if (u.port != "80")
    {
        req += ':';
        req += u.port;
    }
    req += "\r\nAccept: text/event-stream\r\nCache-Control: no-cache\r\n";
    if (!m_authToken.empty())
    {
        req += "Authorization: Bearer ";
        req += m_authToken;
        req += "\r\n";
    }
    req += "Connection: keep-alive\r\n\r\n";

    if (::send(sock, req.data(), static_cast<int>(req.size()), 0) < 0)
    {
        closeSocket(sock);
        return false;
    }

    // Read response headers — accumulate until we see "\r\n\r\n".
    std::string buf;
    buf.reserve(2048);
    char chunk[2048];
    while (m_running.load())
    {
        int n = ::recv(sock, chunk, sizeof(chunk), 0);
        if (n <= 0)
        {
            closeSocket(sock);
            return false;
        }
        buf.append(chunk, static_cast<size_t>(n));
        if (buf.find("\r\n\r\n") != std::string::npos) break;
        if (buf.size() > 16 * 1024) { closeSocket(sock); return false; }
    }
    if (!m_running.load()) { closeSocket(sock); return true; }

    size_t headerEnd = buf.find("\r\n\r\n");
    std::string headerSection = buf.substr(0, headerEnd);
    // Status line check — accept any 2xx.
    {
        size_t sp = headerSection.find(' ');
        if (sp == std::string::npos) { closeSocket(sock); return false; }
        int code = std::atoi(headerSection.c_str() + sp + 1);
        if (code < 200 || code >= 300) { closeSocket(sock); return false; }
    }
    // Content-Type check — must be text/event-stream for SSE.
    {
        std::string lower = headerSection;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        if (lower.find("text/event-stream") == std::string::npos)
        {
            // Server responded with the old one-shot JSON snapshot — parse it
            // and dispatch, then return false so the caller falls back to
            // short-polling (since this server doesn't support SSE).
            std::string body = buf.substr(headerEnd + 4);
            Snapshot snap;
            if (parseSnapshotJSON(body, snap))
            {
                dispatchIfChanged(snap);
            }
            closeSocket(sock);
            return false;
        }
    }

    LOG_INFO("RemoteMetaSync: SSE stream established with " + m_baseUrl);

    std::string body = buf.substr(headerEnd + 4);
    buf.clear();

    auto processEvents = [&](std::string &stream)
    {
        size_t eventEnd;
        while ((eventEnd = stream.find("\n\n")) != std::string::npos)
        {
            std::string event = stream.substr(0, eventEnd);
            stream.erase(0, eventEnd + 2);

            std::string data;
            size_t pos = 0;
            while (pos < event.size())
            {
                size_t lineEnd = event.find('\n', pos);
                std::string line = (lineEnd == std::string::npos)
                                       ? event.substr(pos)
                                       : event.substr(pos, lineEnd - pos);
                pos = (lineEnd == std::string::npos) ? event.size() : lineEnd + 1;
                // Strip CR if present.
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line[0] == ':') continue; // blank or comment
                if (line.compare(0, 5, "data:") == 0)
                {
                    std::string d = line.substr(5);
                    if (!d.empty() && d.front() == ' ') d.erase(0, 1);
                    if (!data.empty()) data += '\n';
                    data += d;
                }
                // Other SSE fields (event:, id:, retry:) are ignored.
            }

            if (!data.empty())
            {
                Snapshot snap;
                if (parseSnapshotJSON(data, snap))
                {
                    dispatchIfChanged(snap);
                }
            }
        }
    };

    processEvents(body);

    while (m_running.load())
    {
        int n = ::recv(sock, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        body.append(chunk, static_cast<size_t>(n));
        if (body.size() > 256 * 1024)
        {
            // Sanity cap. Drop anything before the last complete event boundary.
            size_t lastSep = body.rfind("\n\n");
            body = (lastSep == std::string::npos) ? std::string{} : body.substr(lastSep + 2);
        }
        processEvents(body);
    }

    closeSocket(sock);
    return true;
}

void RemoteMetaSync::pollLoop()
{
    // SSE-first strategy: try the long-poll, fall back to short-poll when
    // the server doesn't speak event-stream. Reconnect with backoff on
    // either failure mode.
    int sseFailureStreak = 0;
    while (m_running.load())
    {
        if (runSSE())
        {
            // Stream ended — assume transient (network blip, server
            // restart). Reconnect after a short pause.
            sseFailureStreak = 0;
        }
        else
        {
            // SSE not supported / connect failure / parse error.
            sseFailureStreak++;
            if (sseFailureStreak == 1)
            {
                LOG_INFO("RemoteMetaSync: SSE unavailable, falling back to short-poll");
            }
            // One short-poll round to keep deltas flowing.
            Snapshot snap;
            if (fetchOnce(snap))
            {
                dispatchIfChanged(snap);
            }
        }

        // Reconnect / re-poll cadence: shorter for SSE retries (likely
        // transient), longer for failing short-polls (likely peer down).
        const int sleepMs = (sseFailureStreak > 3) ? m_pollIntervalMs : 500;
        int remaining = sleepMs;
        while (remaining > 0 && m_running.load())
        {
            const int slice = std::min(remaining, 100);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            remaining -= slice;
        }
    }
}
