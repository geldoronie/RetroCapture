#include "HttpClient.h"

#include <cctype>
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

    // Minimal http:// parser, mirrors the one in RemoteMetaSync.cpp.
    // No urlencoding, no userinfo, no fragment — strictly enough URL
    // to address our directory service.
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

    void setRecvTimeout(socket_t sock, int ms)
    {
#ifdef _WIN32
        DWORD tv = static_cast<DWORD>(ms);
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char *>(&tv), sizeof(tv));
#else
        timeval tv{};
        tv.tv_sec  = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    // Case-insensitive find of a header field in the raw response.
    // Returns the trimmed value or empty when missing.
    std::string getHeaderValue(const std::string &response, const std::string &fieldName)
    {
        std::string lowerResp;
        lowerResp.reserve(response.size());
        for (char c : response) lowerResp.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

        std::string needle = "\r\n";
        needle.reserve(needle.size() + fieldName.size() + 1);
        for (char c : fieldName) needle.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        needle.push_back(':');

        size_t pos = lowerResp.find(needle);
        if (pos == std::string::npos) return {};

        size_t valStart = pos + needle.size();
        size_t valEnd   = lowerResp.find("\r\n", valStart);
        if (valEnd == std::string::npos) return {};

        std::string value = response.substr(valStart, valEnd - valStart);
        // Trim leading whitespace.
        size_t i = 0;
        while (i < value.size() && (value[i] == ' ' || value[i] == '\t')) ++i;
        // Trim trailing whitespace.
        size_t j = value.size();
        while (j > i && (value[j - 1] == ' ' || value[j - 1] == '\t' || value[j - 1] == '\r' || value[j - 1] == '\n')) --j;
        return value.substr(i, j - i);
    }
}

const char *HttpClient::methodName(Method m)
{
    switch (m)
    {
        case Method::GET:     return "GET";
        case Method::POST:    return "POST";
        case Method::PATCH:   return "PATCH";
        case Method::DELETE_: return "DELETE";
    }
    return "GET";
}

HttpClient::Response HttpClient::send(Method method,
                                      const std::string &url,
                                      const std::string &jsonBody,
                                      int recvTimeoutMs)
{
    Response resp;

    UrlParts u;
    if (!parseHttpUrl(url, u))
    {
        resp.error = "malformed URL (expected http://host[:port][/path]): " + url;
        return resp;
    }

    socket_t sock = connectTcp(u);
    if (sock == INVALID_SOCK)
    {
        resp.error = "connect to " + u.host + ":" + u.port + " failed";
        return resp;
    }
    setRecvTimeout(sock, recvTimeoutMs);

    // Build request.
    std::string req;
    req.reserve(256 + jsonBody.size());
    req += methodName(method);
    req += ' ';
    req += u.path;
    req += " HTTP/1.1\r\nHost: ";
    req += u.host;
    if (u.port != "80")
    {
        req += ':';
        req += u.port;
    }
    req += "\r\nConnection: close\r\nAccept: application/json\r\n";
    if (!jsonBody.empty())
    {
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: ";
        req += std::to_string(jsonBody.size());
        req += "\r\n";
    }
    req += "\r\n";
    if (!jsonBody.empty()) req += jsonBody;

    if (::send(sock, req.data(), static_cast<int>(req.size()), 0) < 0)
    {
        closeSocket(sock);
        resp.error = "send failed";
        return resp;
    }

    // Read until close or sanity cap. Directory responses are tiny
    // (a few hundred bytes at most), so 256 KB is far more than
    // enough.
    std::string response;
    response.reserve(4096);
    char buf[2048];
    for (;;)
    {
        int n = ::recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        response.append(buf, static_cast<size_t>(n));
        if (response.size() > 256 * 1024) break;
    }
    closeSocket(sock);

    // Parse status line.
    size_t sep = response.find("\r\n\r\n");
    if (sep == std::string::npos)
    {
        resp.error = "no response headers (got " + std::to_string(response.size()) + " bytes)";
        return resp;
    }
    const std::string statusLine = response.substr(0, response.find("\r\n"));
    size_t firstSpace = statusLine.find(' ');
    if (firstSpace == std::string::npos)
    {
        resp.error = "malformed status line: " + statusLine;
        return resp;
    }
    resp.statusCode = std::atoi(statusLine.c_str() + firstSpace + 1);
    if (resp.statusCode <= 0)
    {
        resp.error = "could not parse status code from: " + statusLine;
        return resp;
    }
    resp.ok = true;
    resp.body = response.substr(sep + 4);
    resp.retryAfter = getHeaderValue(response.substr(0, sep), "Retry-After");
    return resp;
}
