#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Minimal synchronous HTTP/1.1 client.
 *
 * Built directly on top of sockets (winsock on Windows, POSIX
 * elsewhere) so we don't pull in libcurl just for a few JSON POSTs.
 * Supports GET / POST / PATCH / DELETE with an optional JSON body
 * and a configurable receive timeout.
 *
 * Synchronous and intentionally blocking — callers run it on a
 * background thread when they don't want to stall the main loop.
 * The directory-publish path in src/streaming/DirectoryClient is
 * the canonical caller.
 *
 * No TLS. Use this only for plaintext HTTP endpoints under our
 * control (local directory service, dev tunnels). For HTTPS we'd
 * need libcurl + OpenSSL, which we deliberately avoid in this
 * binary.
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
     * @brief Send one request, return the response.
     *
     * @param method        HTTP method.
     * @param url           Absolute URL ("http://host[:port]/path"). https:// is not supported.
     * @param jsonBody      Body to send; pass empty string to send no body. The
     *                      Content-Type header is set to application/json when non-empty.
     * @param recvTimeoutMs Receive-side timeout. Connect uses ~the OS default;
     *                      a remote that accepts but never responds will be
     *                      cut after this many ms.
     */
    static Response send(Method method,
                         const std::string &url,
                         const std::string &jsonBody = "",
                         int recvTimeoutMs = 5000);

    /// Returns "GET" / "POST" / "PATCH" / "DELETE".
    static const char *methodName(Method m);
};
