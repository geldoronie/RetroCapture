#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Minimal synchronous HTTP/1.1 + HTTPS client.
 *
 * Built directly on top of sockets (winsock on Windows, POSIX
 * elsewhere). HTTPS uses OpenSSL when the build links it in
 * (CMakeLists.txt sets `ENABLE_HTTPS` when OpenSSL is detected,
 * which is the case on every supported target).
 *
 * Synchronous and intentionally blocking — callers run it on a
 * background thread when they don't want to stall the main loop.
 * The directory-publish path in src/streaming/DirectoryClient is
 * the canonical caller.
 *
 * Use this for any short-lived request/response (directory ops,
 * /meta polls, report submission). Long-lived streaming connections
 * (VideoCaptureRemote consuming /raw) own their own socket lifecycle
 * but share the same TLS helper functions through HttpClientTls.h.
 */
class HttpClient
{
public:
    enum class Method
    {
        GET,
        POST,
        PATCH,
        DELETE_,   // suffix because DELETE is a winsock macro on Windows
    };

    struct Response
    {
        bool        ok          = false;   // transport-level success (got a status line)
        int         statusCode  = 0;       // HTTP status, 0 if no response
        std::string body;
        std::string retryAfter;            // value of Retry-After header, empty if absent
        std::string error;                 // human-readable failure when ok==false
    };

    /**
     * Per-request options. Defaults match the historical behaviour of
     * the four-arg send() so callers don't need to touch anything.
     */
    struct Options
    {
        int  recvTimeoutMs      = 5000;
        /**
         * When true, the TLS handshake skips peer-certificate
         * verification. Use ONLY for developer testing against a
         * self-signed cert — never in shipping code paths that touch
         * the public directory.
         *
         * No effect on plain http:// URLs.
         */
        bool insecureSkipVerify = false;
    };

    /**
     * @brief Send one request, return the response.
     *
     * @param method   HTTP method.
     * @param url      Absolute URL ("http://host[:port][/path]" or
     *                 "https://host[:port][/path]"). https:// requires
     *                 the build to have OpenSSL linked (always true on
     *                 supported targets); without it, the call fails
     *                 with `error = "https not supported in this build"`.
     * @param jsonBody Body to send; pass empty string to send no body. The
     *                 Content-Type header is set to application/json when non-empty.
     */
    static Response send(Method method,
                         const std::string &url,
                         const std::string &jsonBody,
                         const Options &options);

    /// Convenience overload that uses default Options (5 s timeout,
    /// peer verification on for HTTPS). Preserves the four-arg
    /// signature callers across the codebase have been using since
    /// before TLS landed.
    static Response send(Method method,
                         const std::string &url,
                         const std::string &jsonBody = "",
                         int recvTimeoutMs = 5000);

    /// Returns "GET" / "POST" / "PATCH" / "DELETE".
    static const char *methodName(Method m);
};
