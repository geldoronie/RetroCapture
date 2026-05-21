#include "HttpClient.h"
#include "HttpClientTls.h"

#include <cctype>
#include <cstring>

using httpinternal::Connection;
using httpinternal::INVALID_SOCK;
using httpinternal::parseHttpUrl;
using httpinternal::setRecvTimeout;
using httpinternal::UrlParts;
#ifdef ENABLE_HTTPS
using httpinternal::establishTls;
#endif
using httpinternal::connectTcp;

namespace
{
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
    // (a few hundred bytes at most), so 256 KB is far more than enough.
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
