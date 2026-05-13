#include "RemoteMetaSync.h"
#include "../utils/Logger.h"

#include <nlohmann/json.hpp>

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

    // Synchronous HTTP/1.1 GET with a 5-second receive deadline. Returns
    // the response body on 2xx, empty string otherwise. Logs errors but
    // doesn't throw — callers treat empty as "skip this poll".
    std::string httpGet(const std::string &fullUrl)
    {
        UrlParts u;
        if (!parseHttpUrl(fullUrl, u))
        {
            LOG_WARN("RemoteMetaSync: malformed URL: " + fullUrl);
            return {};
        }

        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *res = nullptr;
        if (getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res) != 0 || !res)
        {
            return {};
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
        req += "\r\nConnection: close\r\nAccept: application/json\r\n\r\n";

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
    std::string body = httpGet(m_baseUrl + "/meta");
    if (body.empty()) return false;

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
                    ParamOverride po;
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
            out.imageBrightness = im.value("brightness", 1.0f);
            out.imageContrast   = im.value("contrast",   1.0f);
        }

        return true;
    }
    catch (const std::exception &e)
    {
        LOG_WARN(std::string("RemoteMetaSync: JSON parse failed — ") + e.what());
        return false;
    }
}

void RemoteMetaSync::pollLoop()
{
    while (m_running.load())
    {
        Snapshot snap;
        if (fetchOnce(snap))
        {
            // Build a value vector for the parameter dedup check.
            std::vector<float> currentValues;
            currentValues.reserve(snap.parameters.size());
            for (const auto &p : snap.parameters)
            {
                currentValues.push_back(p.value);
            }

            const bool presetChanged  = (snap.preset     != m_lastPreset);
            const bool hashChanged    = (snap.presetHash != m_lastPresetHash);
            const bool toggleChanged  = (snap.pipelineEnabled != m_lastPipelineEnabled);
            const bool paramsChanged  = (currentValues != m_lastParamValues);

            if (presetChanged || hashChanged || toggleChanged || paramsChanged)
            {
                if (m_cb) m_cb(snap);
                m_lastPreset          = snap.preset;
                m_lastPresetHash      = snap.presetHash;
                m_lastPipelineEnabled = snap.pipelineEnabled;
                m_lastParamValues     = std::move(currentValues);
            }
        }

        // Sleep in 100 ms slices so stop() is responsive without resorting
        // to condition variables for this very-low-rate poller.
        int remaining = m_pollIntervalMs;
        while (remaining > 0 && m_running.load())
        {
            const int slice = std::min(remaining, 100);
            std::this_thread::sleep_for(std::chrono::milliseconds(slice));
            remaining -= slice;
        }
    }
}
