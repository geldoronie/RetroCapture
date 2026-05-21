#include "HttpAuth.h"

#include "PasswordHash.h"

#include <cctype>

namespace
{
    // Case-insensitive substring search starting at `from`. Returns
    // string::npos when not found.
    size_t findCI(const std::string &haystack, const std::string &needle, size_t from = 0)
    {
        if (needle.empty() || haystack.size() < needle.size()) return std::string::npos;
        for (size_t i = from; i + needle.size() <= haystack.size(); ++i)
        {
            bool match = true;
            for (size_t j = 0; j < needle.size(); ++j)
            {
                char a = static_cast<char>(std::tolower(static_cast<unsigned char>(haystack[i + j])));
                char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
                if (a != b) { match = false; break; }
            }
            if (match) return i;
        }
        return std::string::npos;
    }

    std::string trim(const std::string &s)
    {
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        size_t j = s.size();
        while (j > i && (s[j - 1] == ' ' || s[j - 1] == '\t' ||
                         s[j - 1] == '\r' || s[j - 1] == '\n'))
        {
            --j;
        }
        return s.substr(i, j - i);
    }
}

namespace HttpAuth
{
    std::string extractBearerToken(const std::string &rawRequest)
    {
        // ── Header path: Authorization: Bearer <token>
        //
        // We search only within the headers (everything before the
        // CRLF CRLF separator) to avoid matching the literal string
        // "Authorization:" inside a request body — not a real attack,
        // but ill-formed bodies shouldn't be able to spoof auth.
        size_t headerEnd = rawRequest.find("\r\n\r\n");
        const std::string headers = (headerEnd == std::string::npos)
                                        ? rawRequest
                                        : rawRequest.substr(0, headerEnd);

        size_t authPos = findCI(headers, "\r\nauthorization:");
        if (authPos == std::string::npos &&
            findCI(headers, "authorization:") == 0)
        {
            authPos = 0;
        }
        if (authPos != std::string::npos)
        {
            // Skip past the field name to the value.
            size_t colon = headers.find(':', authPos);
            if (colon != std::string::npos)
            {
                size_t lineEnd = headers.find("\r\n", colon);
                if (lineEnd == std::string::npos) lineEnd = headers.size();
                std::string value = trim(headers.substr(colon + 1, lineEnd - colon - 1));

                // Expect "Bearer <token>". Case-insensitive on the
                // scheme name.
                if (value.size() > 7)
                {
                    std::string scheme = value.substr(0, 6);
                    for (auto &c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (scheme == "bearer" && (value[6] == ' ' || value[6] == '\t'))
                    {
                        return trim(value.substr(7));
                    }
                }
            }
        }

        // ── Query-string fallback: ?token=<value> or &token=<value>
        //
        // Inspect only the request line (first CRLF-terminated line).
        size_t reqLineEnd = rawRequest.find("\r\n");
        if (reqLineEnd == std::string::npos) reqLineEnd = rawRequest.size();
        const std::string reqLine = rawRequest.substr(0, reqLineEnd);

        size_t q = reqLine.find('?');
        if (q != std::string::npos)
        {
            size_t spaceAfter = reqLine.find(' ', q);
            if (spaceAfter == std::string::npos) spaceAfter = reqLine.size();
            std::string query = reqLine.substr(q + 1, spaceAfter - q - 1);

            // Walk &-separated pairs looking for token=...
            size_t pos = 0;
            while (pos < query.size())
            {
                size_t amp = query.find('&', pos);
                if (amp == std::string::npos) amp = query.size();
                std::string pair = query.substr(pos, amp - pos);
                if (pair.size() > 6 && pair.compare(0, 6, "token=") == 0)
                {
                    return pair.substr(6);
                }
                pos = amp + 1;
            }
        }
        return {};
    }

    bool authorized(const std::string &rawRequest, const std::string &expectedHash)
    {
        if (expectedHash.empty()) return true;
        const std::string got = extractBearerToken(rawRequest);
        if (got.size() != expectedHash.size()) return false;

        // Constant-time compare to avoid leaking the hash via timing.
        unsigned char diff = 0;
        for (size_t i = 0; i < got.size(); ++i)
        {
            diff |= static_cast<unsigned char>(got[i] ^ expectedHash[i]);
        }
        return diff == 0;
    }

    // ── Basic auth path (browser-native popup) ───────────────────────
    //
    // Minimal RFC 4648 base64 decoder. We use it on a Basic header
    // value (<= a few hundred bytes), so a simple table-driven loop
    // is fine.
    static std::string base64Decode(const std::string &in)
    {
        static signed char lut[256];
        static bool        init = false;
        if (!init)
        {
            for (int i = 0; i < 256; ++i) lut[i] = -1;
            const char *alphabet =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            for (int i = 0; i < 64; ++i) lut[static_cast<unsigned char>(alphabet[i])] = static_cast<signed char>(i);
            init = true;
        }

        std::string out;
        out.reserve(in.size() * 3 / 4 + 1);
        int  val = 0;
        int  valb = -8;
        for (unsigned char c : in)
        {
            if (c == '=') break;
            if (lut[c] < 0) continue;        // skip whitespace / padding
            val = (val << 6) | lut[c];
            valb += 6;
            if (valb >= 0)
            {
                out.push_back(static_cast<char>((val >> valb) & 0xFF));
                valb -= 8;
            }
        }
        return out;
    }

    std::string extractBasicPassword(const std::string &rawRequest)
    {
        size_t headerEnd = rawRequest.find("\r\n\r\n");
        const std::string headers = (headerEnd == std::string::npos)
                                        ? rawRequest
                                        : rawRequest.substr(0, headerEnd);

        // Locate "Authorization:" case-insensitively, accepting it as
        // the first line or following a CRLF.
        size_t authPos = findCI(headers, "\r\nauthorization:");
        if (authPos == std::string::npos &&
            findCI(headers, "authorization:") == 0)
        {
            authPos = 0;
        }
        if (authPos == std::string::npos) return {};

        const size_t colon = headers.find(':', authPos);
        if (colon == std::string::npos) return {};
        const size_t lineEnd = headers.find("\r\n", colon);
        const std::string raw = headers.substr(colon + 1,
                                                (lineEnd == std::string::npos ? headers.size() : lineEnd) - (colon + 1));
        const std::string value = trim(raw);

        if (value.size() <= 6) return {};
        std::string scheme = value.substr(0, 5);
        for (auto &c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (scheme != "basic") return {};
        if (value[5] != ' ' && value[5] != '\t') return {};

        const std::string encoded = trim(value.substr(6));
        const std::string decoded = base64Decode(encoded);
        const size_t      sep     = decoded.find(':');
        if (sep == std::string::npos) return {};
        return decoded.substr(sep + 1);    // username discarded
    }

    bool authorizedAnyScheme(const std::string &rawRequest,
                             const std::string &expectedHash)
    {
        if (expectedHash.empty()) return true;

        // Try Bearer / ?token=... first — that's the path the
        // RetroCapture client uses and it short-circuits without
        // ever invoking sha256.
        if (authorized(rawRequest, expectedHash)) return true;

        // Fall through to Basic. The user typed a plaintext password
        // into the browser's native popup; hash it the same way the
        // host hashed the configured value and compare.
        const std::string pw = extractBasicPassword(rawRequest);
        if (pw.empty()) return false;
        const std::string hash = PasswordHash::sha256Hex(pw);
        if (hash.size() != expectedHash.size()) return false;
        unsigned char diff = 0;
        for (size_t i = 0; i < hash.size(); ++i)
        {
            diff |= static_cast<unsigned char>(hash[i] ^ expectedHash[i]);
        }
        return diff == 0;
    }
}
