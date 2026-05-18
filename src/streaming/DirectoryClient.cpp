#include "DirectoryClient.h"

#include "../utils/HttpClient.h"
#include "../utils/Logger.h"

#include <nlohmann/json.hpp>

#include <algorithm>

using json = nlohmann::json;

namespace
{
    // Strip trailing slash so baseUrl + "/register" doesn't end up
    // with "//register".
    std::string trimSlash(std::string u)
    {
        while (!u.empty() && u.back() == '/') u.pop_back();
        return u;
    }
}

std::string DirectoryClient::Config::validate() const
{
    if (directoryUrl.empty()) return "directory URL is required";
    if (directoryUrl.rfind("http://", 0) != 0)
        return "directory URL must start with http://";
    if (name.empty())          return "stream name is required";
    if (name.size() > 120)     return "stream name too long (max 120)";
    if (hostNickname.size() > 40) return "nickname too long (max 40)";
    if (endpoint.empty())      return "endpoint URL is required";
    if (resolutionW == 0 || resolutionH == 0)
        return "resolution must be non-zero";
    if (fps == 0)              return "fps must be non-zero";
    if (codec != "h264" && codec != "h265")
        return "codec must be h264 or h265";
    if (endpointMode != "direct" && endpointMode != "tunnel-cloudflare" && endpointMode != "custom")
        return "endpointMode must be direct, tunnel-cloudflare or custom";
    return {};
}

DirectoryClient::DirectoryClient() = default;

DirectoryClient::~DirectoryClient()
{
    stop();
}

bool DirectoryClient::start(const Config &cfg)
{
    auto err = cfg.validate();
    if (!err.empty())
    {
        LOG_WARN(std::string("DirectoryClient::start — invalid config: ") + err);
        setError(err);
        return false;
    }

    // start-while-running == reconfigure: tear down the current entry
    // first so we don't leak it server-side, then bring up fresh.
    if (m_state.load() != State::Idle)
    {
        stop();
    }

    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_cfg = cfg;
        m_cfg.directoryUrl = trimSlash(m_cfg.directoryUrl);
        m_pendingPatch = Patch{};
        m_hasPendingPatch = false;
        m_currentClientCount = 0;
        m_streamId.clear();
        m_ownerToken.clear();
        m_lastError.clear();
    }
    // Reset telemetry on every fresh start so the displayed numbers
    // reflect the current session, not history across reconfigures.
    m_registerOk.store(0);
    m_registerFail.store(0);
    m_heartbeatOk.store(0);
    m_heartbeatFail.store(0);
    m_patchOk.store(0);
    m_patchFail.store(0);
    m_lastHeartbeatSteadyMs.store(-1);
    m_stopRequested.store(false);
    m_state.store(State::Registering);
    m_thread = std::thread([this] { workerLoop(); });
    return true;
}

void DirectoryClient::updateMetadata(const Patch &p)
{
    std::lock_guard<std::mutex> lock(m_mu);
    // Merge: later patches overwrite earlier ones field-by-field. The
    // worker thread drains all of them at once before going back to
    // sleep, so we lose no information by squashing.
    if (!p.name.empty())          m_pendingPatch.name = p.name;
    if (!p.hostNickname.empty())  m_pendingPatch.hostNickname = p.hostNickname;
    if (!p.shader.empty())        m_pendingPatch.shader = p.shader;
    if (p.resolutionW != 0)       m_pendingPatch.resolutionW = p.resolutionW;
    if (p.resolutionH != 0)       m_pendingPatch.resolutionH = p.resolutionH;
    if (p.fps != 0)               m_pendingPatch.fps = p.fps;
    if (!p.codec.empty())         m_pendingPatch.codec = p.codec;
    if (p.setPasswordRequired)
    {
        m_pendingPatch.setPasswordRequired = true;
        m_pendingPatch.passwordRequired    = p.passwordRequired;
    }
    if (!p.endpoint.empty())      m_pendingPatch.endpoint = p.endpoint;
    if (!p.endpointMode.empty())  m_pendingPatch.endpointMode = p.endpointMode;
    m_hasPendingPatch = true;
    m_cv.notify_all();
}

void DirectoryClient::setClientCount(int n)
{
    if (n < 0) n = 0;
    std::lock_guard<std::mutex> lock(m_mu);
    m_currentClientCount = n;
}

void DirectoryClient::stop()
{
    if (m_state.load() == State::Idle && !m_thread.joinable()) return;

    m_stopRequested.store(true);
    m_cv.notify_all();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    m_state.store(State::Idle);
}

std::string DirectoryClient::getStreamId() const
{
    std::lock_guard<std::mutex> lock(m_mu);
    return m_streamId;
}

std::string DirectoryClient::getLastError() const
{
    std::lock_guard<std::mutex> lock(m_mu);
    return m_lastError;
}

DirectoryClient::Stats DirectoryClient::getStats() const
{
    Stats s;
    s.registerOk    = m_registerOk.load();
    s.registerFail  = m_registerFail.load();
    s.heartbeatOk   = m_heartbeatOk.load();
    s.heartbeatFail = m_heartbeatFail.load();
    s.patchOk       = m_patchOk.load();
    s.patchFail     = m_patchFail.load();

    const int64_t lastMs = m_lastHeartbeatSteadyMs.load();
    if (lastMs >= 0)
    {
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch()).count();
        s.secondsSinceLastHeartbeat = (nowMs - lastMs) / 1000;
    }
    return s;
}

void DirectoryClient::setError(const std::string &msg)
{
    std::lock_guard<std::mutex> lock(m_mu);
    m_lastError = msg;
}

void DirectoryClient::workerLoop()
{
    auto backoff = kInitialBackoff;

    // ── Initial register loop. We don't give up — the user toggled
    // "publish" and expects us to keep trying. Backoff caps at 60 s.
    while (!m_stopRequested.load())
    {
        if (doRegister())
        {
            m_state.store(State::Active);
            backoff = kInitialBackoff;
            break;
        }
        m_state.store(State::Error);
        std::unique_lock<std::mutex> lock(m_mu);
        if (m_cv.wait_for(lock, backoff, [this] { return m_stopRequested.load(); }))
        {
            break; // stop requested during backoff
        }
        backoff = std::min(backoff * 2, kMaxBackoff);
    }

    if (m_stopRequested.load())
    {
        m_state.store(State::Stopping);
        doDelete();
        m_state.store(State::Idle);
        return;
    }

    // ── Steady state: heartbeat every 30 s, drain pending patches as
    // they arrive. The condition_variable wait wakes us early either
    // for a patch or for stop.
    auto lastHeartbeat = std::chrono::steady_clock::now();
    backoff = kInitialBackoff;

    while (!m_stopRequested.load())
    {
        std::unique_lock<std::mutex> lock(m_mu);
        auto until = lastHeartbeat + kHeartbeatInterval;
        m_cv.wait_until(lock, until, [this] {
            return m_stopRequested.load() || m_hasPendingPatch;
        });
        bool stopNow = m_stopRequested.load();
        bool patchNow = m_hasPendingPatch;
        lock.unlock();

        if (stopNow) break;

        if (patchNow)
        {
            if (doPatchIfPending())
            {
                m_state.store(State::Active);
                backoff = kInitialBackoff;
            }
            else
            {
                m_state.store(State::Error);
            }
        }

        // Heartbeat if it's time.
        if (std::chrono::steady_clock::now() >= until)
        {
            if (doHeartbeat())
            {
                lastHeartbeat = std::chrono::steady_clock::now();
                m_state.store(State::Active);
                backoff = kInitialBackoff;
            }
            else
            {
                m_state.store(State::Error);
                // Sleep `backoff` before next attempt instead of the
                // full 30 s, so we recover quickly from transient
                // hiccups.
                std::unique_lock<std::mutex> back(m_mu);
                if (m_cv.wait_for(back, backoff, [this] { return m_stopRequested.load(); }))
                    break;
                backoff = std::min(backoff * 2, kMaxBackoff);
                lastHeartbeat = std::chrono::steady_clock::now(); // reset so next heartbeat counts from now
            }
        }
    }

    m_state.store(State::Stopping);
    doDelete();
    m_state.store(State::Idle);
}

bool DirectoryClient::doRegister()
{
    Config cfg;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        cfg = m_cfg;
    }

    json body = {
        {"name",             cfg.name},
        {"hostNickname",     cfg.hostNickname},
        {"shader",           cfg.shader},
        {"resolution",       {{"w", cfg.resolutionW}, {"h", cfg.resolutionH}}},
        {"fps",              cfg.fps},
        {"codec",            cfg.codec},
        {"passwordRequired", cfg.passwordRequired},
        {"endpoint",         cfg.endpoint},
        {"endpointMode",     cfg.endpointMode},
        {"version",          cfg.version},
    };

    auto resp = HttpClient::send(HttpClient::Method::POST,
                                 cfg.directoryUrl + "/register",
                                 body.dump());
    if (!resp.ok)
    {
        setError("register transport failure: " + resp.error);
        LOG_WARN("DirectoryClient::doRegister — " + resp.error);
        m_registerFail.fetch_add(1);
        return false;
    }
    if (resp.statusCode != 201)
    {
        setError("register HTTP " + std::to_string(resp.statusCode));
        LOG_WARN("DirectoryClient::doRegister — HTTP " + std::to_string(resp.statusCode) +
                 " body: " + resp.body.substr(0, 256));
        m_registerFail.fetch_add(1);
        return false;
    }

    try
    {
        auto j = json::parse(resp.body);
        auto data = j.at("data");
        std::lock_guard<std::mutex> lock(m_mu);
        m_streamId   = data.at("streamId").get<std::string>();
        m_ownerToken = data.at("ownerToken").get<std::string>();
        m_lastError.clear();
    }
    catch (const std::exception &e)
    {
        setError(std::string("register parse failure: ") + e.what());
        LOG_WARN(std::string("DirectoryClient::doRegister — parse: ") + e.what());
        m_registerFail.fetch_add(1);
        return false;
    }

    m_registerOk.fetch_add(1);
    LOG_INFO("DirectoryClient: registered streamId=" + m_streamId);
    return true;
}

bool DirectoryClient::doHeartbeat()
{
    std::string baseUrl, streamId, ownerToken;
    int cc;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        baseUrl    = m_cfg.directoryUrl;
        streamId   = m_streamId;
        ownerToken = m_ownerToken;
        cc         = m_currentClientCount;
    }
    if (streamId.empty() || ownerToken.empty()) return false;

    json body = {
        {"streamId",    streamId},
        {"ownerToken",  ownerToken},
        {"clientCount", cc},
    };
    auto resp = HttpClient::send(HttpClient::Method::POST,
                                 baseUrl + "/heartbeat",
                                 body.dump());
    if (!resp.ok)
    {
        setError("heartbeat transport: " + resp.error);
        m_heartbeatFail.fetch_add(1);
        return false;
    }
    if (resp.statusCode == 404)
    {
        // The server expired our entry. Force a re-register on next
        // loop iteration by clearing our id and dropping back to
        // Registering.
        LOG_WARN("DirectoryClient: heartbeat 404 — server expired our entry, will re-register");
        m_heartbeatFail.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_streamId.clear();
            m_ownerToken.clear();
        }
        // Trigger a re-register by trying it now; the next workerLoop
        // tick will go through Active again if that works.
        return doRegister();
    }
    if (resp.statusCode != 200)
    {
        setError("heartbeat HTTP " + std::to_string(resp.statusCode));
        m_heartbeatFail.fetch_add(1);
        return false;
    }
    setError("");
    m_heartbeatOk.fetch_add(1);
    m_lastHeartbeatSteadyMs.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    return true;
}

bool DirectoryClient::doPatchIfPending()
{
    Patch p;
    std::string baseUrl, streamId, ownerToken;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        if (!m_hasPendingPatch) return true;
        p = m_pendingPatch;
        m_pendingPatch = Patch{};
        m_hasPendingPatch = false;
        baseUrl    = m_cfg.directoryUrl;
        streamId   = m_streamId;
        ownerToken = m_ownerToken;
    }
    if (streamId.empty() || ownerToken.empty()) return false;

    json body = {{"ownerToken", ownerToken}};
    if (!p.name.empty())         body["name"]         = p.name;
    if (!p.hostNickname.empty()) body["hostNickname"] = p.hostNickname;
    if (!p.shader.empty())       body["shader"]       = p.shader;
    if (p.resolutionW != 0 && p.resolutionH != 0)
        body["resolution"] = {{"w", p.resolutionW}, {"h", p.resolutionH}};
    if (p.fps != 0)              body["fps"]          = p.fps;
    if (!p.codec.empty())        body["codec"]        = p.codec;
    if (p.setPasswordRequired)   body["passwordRequired"] = p.passwordRequired;
    if (!p.endpoint.empty())     body["endpoint"]     = p.endpoint;
    if (!p.endpointMode.empty()) body["endpointMode"] = p.endpointMode;

    auto resp = HttpClient::send(HttpClient::Method::PATCH,
                                 baseUrl + "/streams/" + streamId,
                                 body.dump());
    if (!resp.ok)
    {
        setError("patch transport: " + resp.error);
        m_patchFail.fetch_add(1);
        return false;
    }
    if (resp.statusCode != 204)
    {
        setError("patch HTTP " + std::to_string(resp.statusCode));
        m_patchFail.fetch_add(1);
        return false;
    }
    setError("");
    m_patchOk.fetch_add(1);
    return true;
}

bool DirectoryClient::doDelete()
{
    std::string baseUrl, streamId, ownerToken;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        baseUrl    = m_cfg.directoryUrl;
        streamId   = m_streamId;
        ownerToken = m_ownerToken;
    }
    if (streamId.empty() || ownerToken.empty())
    {
        // Nothing to clean up — we never registered (or already cleaned).
        return true;
    }

    json body = {{"ownerToken", ownerToken}};
    auto resp = HttpClient::send(HttpClient::Method::DELETE_,
                                 baseUrl + "/streams/" + streamId,
                                 body.dump());
    if (!resp.ok)
    {
        LOG_WARN("DirectoryClient::doDelete — transport: " + resp.error);
        // Even on transport failure, the server-side entry will
        // TTL-out within 2 min. Not a real problem.
    }
    else if (resp.statusCode != 204)
    {
        LOG_WARN("DirectoryClient::doDelete — HTTP " + std::to_string(resp.statusCode));
    }
    else
    {
        LOG_INFO("DirectoryClient: deleted streamId=" + streamId);
    }
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_streamId.clear();
        m_ownerToken.clear();
    }
    return true;
}
