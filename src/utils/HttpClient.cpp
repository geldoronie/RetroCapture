#include "HttpClient.h"

#include <cctype>
#include <cstring>
#include <mutex>

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

#ifdef ENABLE_HTTPS
  #include <openssl/ssl.h>
  #include <openssl/err.h>
#endif

namespace
{
    struct UrlParts
    {
        std::string host;
        std::string port;
        std::string path;
        bool        tls = false;
    };

    // Accepts both http:// and https://. No urlencoding, no userinfo,
    // no fragment — strictly enough URL to address our directory
    // service and host /meta endpoints.
    bool parseHttpUrl(const std::string &url, UrlParts &out)
    {
        size_t schemeEnd = 0;
        if (url.compare(0, 8, "https://") == 0)
        {
            out.tls = true;
            schemeEnd = 8;
        }
        else if (url.compare(0, 7, "http://") == 0)
        {
            out.tls = false;
            schemeEnd = 7;
        }
        else
        {
            return false;
        }

        std::string rest = url.substr(schemeEnd);
        size_t pathStart = rest.find('/');
        std::string hostPort = (pathStart == std::string::npos) ? rest : rest.substr(0, pathStart);
        out.path = (pathStart == std::string::npos) ? "/" : rest.substr(pathStart);

        size_t colon = hostPort.find(':');
        if (colon == std::string::npos)
        {
            out.host = hostPort;
            out.port = out.tls ? "443" : "80";
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

#ifdef ENABLE_HTTPS
    // Process-wide TLS client context. Lazy-built on first use, kept
    // alive for the lifetime of the process (cost of building the
    // context dwarfs any per-request allocation).
    //
    // Verification is enabled at the CTX level so any SSL created
    // from it inherits the default. Per-request opt-out flips it to
    // SSL_VERIFY_NONE on the SSL object — never on the shared CTX —
    // so an insecureSkipVerify=true call doesn't leak into the next
    // verified call.
    SSL_CTX *getSharedClientSslCtx()
    {
        static std::once_flag once;
        static SSL_CTX *ctx = nullptr;
        std::call_once(once, []() {
            ctx = SSL_CTX_new(TLS_client_method());
            if (!ctx) return;
            // TLS 1.2 minimum. The 1.0/1.1 deprecation has been
            // accepted by the wider ecosystem long enough that we
            // don't need to support them for our own directory.
            SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
            // Trust the OS bundle by default (/etc/ssl/certs on
            // typical Linux, the OS root store on Windows via the
            // Microsoft trust list shipped with OpenSSL builds).
            SSL_CTX_set_default_verify_paths(ctx);
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        });
        return ctx;
    }

    // Wraps an already-connected TCP socket in TLS. On success the
    // caller owns `outSsl` and must free it via SSL_shutdown+SSL_free
    // when done (closeConnection() does that).
    bool establishTls(socket_t sock,
                      const std::string &host,
                      bool skipVerify,
                      SSL *&outSsl,
                      std::string &errOut)
    {
        SSL_CTX *ctx = getSharedClientSslCtx();
        if (!ctx)
        {
            errOut = "SSL_CTX_new failed";
            return false;
        }

        SSL *ssl = SSL_new(ctx);
        if (!ssl)
        {
            errOut = "SSL_new failed";
            return false;
        }

        if (skipVerify)
        {
            SSL_set_verify(ssl, SSL_VERIFY_NONE, nullptr);
        }
        else
        {
            // Hostname verification on top of chain-of-trust. Without
            // this the cert's CN/SAN isn't matched against `host`,
            // so any valid cert from anywhere passes — defeating the
            // point of TLS for a directory client.
            if (SSL_set1_host(ssl, host.c_str()) != 1)
            {
                errOut = "SSL_set1_host failed";
                SSL_free(ssl);
                return false;
            }
        }

        // SNI — most vhosted endpoints need this to pick the right
        // certificate. Cloudflare in particular returns a generic
        // "trycloudflare" cert without it.
        SSL_set_tlsext_host_name(ssl, host.c_str());

        if (SSL_set_fd(ssl, static_cast<int>(sock)) != 1)
        {
            errOut = "SSL_set_fd failed";
            SSL_free(ssl);
            return false;
        }

        int r = SSL_connect(ssl);
        if (r != 1)
        {
            int sslErr = SSL_get_error(ssl, r);
            unsigned long e = ERR_get_error();
            char errbuf[256] = {0};
            if (e != 0)
            {
                ERR_error_string_n(e, errbuf, sizeof(errbuf));
            }
            errOut = "SSL_connect failed (ssl_err=" +
                     std::to_string(sslErr) +
                     (errbuf[0] ? "): " : ")") +
                     std::string(errbuf);
            SSL_free(ssl);
            return false;
        }

        outSsl = ssl;
        return true;
    }
#endif // ENABLE_HTTPS

    // Lightweight transport abstraction so the request/response loop
    // doesn't need to branch on socket-vs-SSL on every read/write.
    struct Connection
    {
        socket_t sock = INVALID_SOCK;
#ifdef ENABLE_HTTPS
        SSL *ssl = nullptr;
#endif

        int sendAll(const void *data, size_t len)
        {
#ifdef ENABLE_HTTPS
            if (ssl)
            {
                size_t total = 0;
                while (total < len)
                {
                    int n = SSL_write(ssl, static_cast<const char *>(data) + total,
                                      static_cast<int>(len - total));
                    if (n <= 0) return -1;
                    total += static_cast<size_t>(n);
                }
                return static_cast<int>(total);
            }
#endif
            return ::send(sock, static_cast<const char *>(data),
                          static_cast<int>(len), 0);
        }

        int recvSome(char *buf, size_t cap)
        {
#ifdef ENABLE_HTTPS
            if (ssl)
            {
                return SSL_read(ssl, buf, static_cast<int>(cap));
            }
#endif
            return ::recv(sock, buf, static_cast<int>(cap), 0);
        }

        void close()
        {
#ifdef ENABLE_HTTPS
            if (ssl)
            {
                SSL_shutdown(ssl);
                SSL_free(ssl);
                ssl = nullptr;
            }
#endif
            if (sock != INVALID_SOCK)
            {
                closeSocket(sock);
                sock = INVALID_SOCK;
            }
        }
    };

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
        size_t i = 0;
        while (i < value.size() && (value[i] == ' ' || value[i] == '\t')) ++i;
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
                                      const Options &options)
{
    Response resp;

    UrlParts u;
    if (!parseHttpUrl(url, u))
    {
        resp.error = "malformed URL (expected http(s)://host[:port][/path]): " + url;
        return resp;
    }

#ifndef ENABLE_HTTPS
    if (u.tls)
    {
        resp.error = "https not supported in this build (rebuild with OpenSSL)";
        return resp;
    }
#endif

    Connection c;
    c.sock = connectTcp(u);
    if (c.sock == INVALID_SOCK)
    {
        resp.error = "connect to " + u.host + ":" + u.port + " failed";
        return resp;
    }
    setRecvTimeout(c.sock, options.recvTimeoutMs);

#ifdef ENABLE_HTTPS
    if (u.tls)
    {
        std::string tlsErr;
        if (!establishTls(c.sock, u.host, options.insecureSkipVerify, c.ssl, tlsErr))
        {
            c.close();
            resp.error = tlsErr;
            return resp;
        }
    }
#endif

    // Build request.
    std::string req;
    req.reserve(256 + jsonBody.size());
    req += methodName(method);
    req += ' ';
    req += u.path;
    req += " HTTP/1.1\r\nHost: ";
    req += u.host;
    if ((u.tls && u.port != "443") || (!u.tls && u.port != "80"))
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

    if (c.sendAll(req.data(), req.size()) < 0)
    {
        c.close();
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
        int n = c.recvSome(buf, sizeof(buf));
        if (n <= 0) break;
        response.append(buf, static_cast<size_t>(n));
        if (response.size() > 256 * 1024) break;
    }
    c.close();

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

HttpClient::Response HttpClient::send(Method method,
                                      const std::string &url,
                                      const std::string &jsonBody,
                                      int recvTimeoutMs)
{
    Options opts;
    opts.recvTimeoutMs = recvTimeoutMs;
    return send(method, url, jsonBody, opts);
}
