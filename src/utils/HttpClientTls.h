#pragma once

// Internal HTTP/TLS helpers shared between HttpClient (request/response)
// and the few callers that own their own socket lifecycle — notably
// RemoteMetaSync's SSE long-poll, which needs to hold a connection open
// and read events as they arrive rather than do a one-shot request.
//
// Header-only on purpose: the helpers are tiny, used in two TUs, and
// keeping them inline avoids a third translation unit just for plumbing.
// The `inline` keyword on every function keeps the linker from
// complaining about multiple definitions.
//
// HTTPS support follows the same ENABLE_HTTPS gate the rest of the
// codebase uses — CMake defines it when OpenSSL is detected, which is
// every shipping target.

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <sys/stat.h>

// MinGW-w64's <sys/stat.h> defines _S_IFREG/_S_IFDIR but not always the
// POSIX S_ISREG/S_ISDIR macros — depends on which feature-test flags
// the bundled headers ship with. Define them defensively so this
// header is self-contained on every toolchain we cross-compile with.
#ifndef S_ISREG
  #define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISDIR
  #define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <sys/socket.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
#endif

#ifdef ENABLE_HTTPS
  #include <openssl/ssl.h>
  #include <openssl/err.h>
#endif

namespace httpinternal
{

#ifdef _WIN32
    using socket_t = SOCKET;
    // `static constexpr` (not `inline constexpr`) so this header works
    // on MXE's MinGW-w64 GCC which doesn't ship C++17 inline variables.
    // Each TU gets its own copy with internal linkage — identical value,
    // no ODR issue.
    static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
    inline int closeSocket(socket_t s) { return ::closesocket(s); }
    // Half-close to wake a thread blocked in recv() on this socket from
    // another thread (the socket stays valid; the blocked recv returns).
    inline int shutdownSocket(socket_t s) { return ::shutdown(s, SD_BOTH); }
#else
    using socket_t = int;
    static constexpr socket_t INVALID_SOCK = -1;
    inline int closeSocket(socket_t s) { return ::close(s); }
    inline int shutdownSocket(socket_t s) { return ::shutdown(s, SHUT_RDWR); }
#endif

    struct UrlParts
    {
        std::string host;
        std::string port;
        std::string path;
        bool        tls = false;
    };

    // Accepts both http:// and https://. No urlencoding, no userinfo,
    // no fragment — strictly enough URL to address our directory
    // service and host /meta / SSE endpoints.
    inline bool parseHttpUrl(const std::string &url, UrlParts &out)
    {
        size_t schemeEnd = 0;
        if (url.compare(0, 8, "https://") == 0)
        {
            out.tls   = true;
            schemeEnd = 8;
        }
        else if (url.compare(0, 7, "http://") == 0)
        {
            out.tls   = false;
            schemeEnd = 7;
        }
        else
        {
            return false;
        }

        std::string rest     = url.substr(schemeEnd);
        size_t      pathStart = rest.find('/');
        std::string hostPort  = (pathStart == std::string::npos) ? rest : rest.substr(0, pathStart);
        out.path              = (pathStart == std::string::npos) ? "/" : rest.substr(pathStart);

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

    inline socket_t connectTcp(const UrlParts &u)
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

    inline void setRecvTimeout(socket_t sock, int ms)
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
    // Returns true if `path` is an existing regular file.
    inline bool fileExists(const char *path)
    {
        if (!path || !*path) return false;
        struct stat st{};
        return ::stat(path, &st) == 0 && S_ISREG(st.st_mode);
    }

    // Returns true if `path` is an existing directory.
    inline bool dirExists(const char *path)
    {
        if (!path || !*path) return false;
        struct stat st{};
        return ::stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    }

    // Walks well-known CA bundle locations and loads the first one that
    // exists. Returns true on success.
    //
    // Why this instead of SSL_CTX_set_default_verify_paths(): when the
    // binary ships its own OpenSSL (AppImage, MXE/MinGW Windows build,
    // some Docker layers) the compiled-in default path points to
    // something that exists on the build host but not on the user's
    // system. OpenSSL 3.x then bubbles up
    //   error:16000069:STORE routines::unregistered scheme
    // on connect. Loading an explicit path that we know exists side-
    // steps the whole STORE-URI machinery.
    //
    // SSL_CERT_FILE / SSL_CERT_DIR env vars override the search — same
    // contract curl / git follow, so users who already configured them
    // don't need to change anything.
    inline bool loadSystemCaBundle(SSL_CTX *ctx)
    {
        const char *envFile = std::getenv("SSL_CERT_FILE");
        if (envFile && fileExists(envFile))
        {
            if (SSL_CTX_load_verify_locations(ctx, envFile, nullptr) == 1) return true;
        }
        const char *envDir = std::getenv("SSL_CERT_DIR");
        if (envDir && dirExists(envDir))
        {
            if (SSL_CTX_load_verify_locations(ctx, nullptr, envDir) == 1) return true;
        }

        // Ordered by hit-rate across distros we ship for.
        static const char *kCandidateFiles[] = {
            "/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Arch, Alpine (when ca-certificates pkg installed)
            "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora, RHEL, CentOS, AlmaLinux, Rocky
            "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // RHEL 7+ extracted bundle
            "/etc/ssl/ca-bundle.pem",                            // openSUSE
            "/etc/ssl/cert.pem",                                 // Alpine, macOS, OpenBSD
            "/usr/local/etc/openssl/cert.pem",                   // Homebrew on macOS (Intel)
            "/opt/homebrew/etc/openssl@3/cert.pem",              // Homebrew on macOS (Apple Silicon)
            nullptr,
        };
        for (const char **p = kCandidateFiles; *p; ++p)
        {
            if (fileExists(*p) && SSL_CTX_load_verify_locations(ctx, *p, nullptr) == 1)
            {
                return true;
            }
        }

        static const char *kCandidateDirs[] = {
            "/etc/ssl/certs",
            "/etc/pki/tls/certs",
            "/etc/pki/ca-trust/extracted/pem",
            nullptr,
        };
        for (const char **p = kCandidateDirs; *p; ++p)
        {
            if (dirExists(*p) && SSL_CTX_load_verify_locations(ctx, nullptr, *p) == 1)
            {
                return true;
            }
        }

        // Last resort: ask OpenSSL for its compiled-in defaults. On
        // most systems this is the same as one of the paths above; on
        // AppImage it's what triggers the STORE error, but at that
        // point we've already exhausted the explicit list so the
        // failure mode is identical and the caller can fall back to
        // skipVerify if they choose.
        return SSL_CTX_set_default_verify_paths(ctx) == 1;
    }

    inline SSL_CTX *getSharedClientSslCtx()
    {
        static std::once_flag once;
        static SSL_CTX *ctx = nullptr;
        std::call_once(once, []() {
            ctx = SSL_CTX_new(TLS_client_method());
            if (!ctx) return;
            SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
            loadSystemCaBundle(ctx);
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        });
        return ctx;
    }

    // Wraps an already-connected TCP socket in TLS. On success the
    // caller owns `outSsl` and must free it via SSL_shutdown+SSL_free
    // when done (Connection::close() does that).
    inline bool establishTls(socket_t sock,
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
            if (SSL_set1_host(ssl, host.c_str()) != 1)
            {
                errOut = "SSL_set1_host failed";
                SSL_free(ssl);
                return false;
            }
        }

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

    // Lightweight transport abstraction so a request/response loop or
    // SSE long-poll doesn't need to branch on socket-vs-SSL on every
    // read/write. Move-friendly; copy disabled to keep ownership clear.
    struct Connection
    {
        socket_t sock = INVALID_SOCK;
#ifdef ENABLE_HTTPS
        SSL *ssl = nullptr;
#endif

        Connection()                              = default;
        Connection(const Connection &)            = delete;
        Connection &operator=(const Connection &) = delete;

        Connection(Connection &&o) noexcept
            : sock(o.sock)
#ifdef ENABLE_HTTPS
            , ssl(o.ssl)
#endif
        {
            o.sock = INVALID_SOCK;
#ifdef ENABLE_HTTPS
            o.ssl = nullptr;
#endif
        }

        Connection &operator=(Connection &&o) noexcept
        {
            if (this != &o)
            {
                close();
                sock   = o.sock;
                o.sock = INVALID_SOCK;
#ifdef ENABLE_HTTPS
                ssl   = o.ssl;
                o.ssl = nullptr;
#endif
            }
            return *this;
        }

        ~Connection() { close(); }

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

    // Opens a Connection (TCP + TLS-if-https) to `u`. On failure returns
    // a closed Connection and fills `errOut`. Doesn't apply any
    // recv-timeout — caller decides because SSE wants none and request
    // paths want a few seconds.
    inline Connection openConnection(const UrlParts &u,
                                     bool skipVerify,
                                     std::string &errOut)
    {
        Connection c;
        c.sock = connectTcp(u);
        if (c.sock == INVALID_SOCK)
        {
            errOut = "connect to " + u.host + ":" + u.port + " failed";
            return c;
        }
#ifdef ENABLE_HTTPS
        if (u.tls)
        {
            if (!establishTls(c.sock, u.host, skipVerify, c.ssl, errOut))
            {
                c.close();
                return c;
            }
        }
#else
        if (u.tls)
        {
            errOut = "https not supported in this build (rebuild with OpenSSL)";
            c.close();
            return c;
        }
#endif
        return c;
    }

} // namespace httpinternal
