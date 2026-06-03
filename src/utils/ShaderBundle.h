#pragma once

// #54 — shader bundle transport for directory streams.
//
// A "bundle" is a shader preset (.glslp) plus every file it references
// (.glsl passes + LUT textures), packed into one blob so a remote client
// that doesn't have the host's shader can fetch and reproduce it. This is a
// transport only — the shader format is unchanged; files are carried
// verbatim with their paths relative to a common root so the directory
// layout (and thus the preset's relative references) survives extraction.
//
// Format ("RCSB1"): magic, then for each entry
//   uint32 pathLen (LE) | path bytes | uint64 dataLen (LE) | data bytes
// Header-only and dependency-free so both the host (APIController) and the
// client (Application) share one implementation.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace shaderbundle
{

struct Entry
{
    std::string relPath; // path relative to the bundle root (POSIX '/')
    std::string data;    // raw file bytes
};

// Magic prefix "RCSB1". A function (not an inline constexpr array, which the
// project's older MinGW toolchain rejects) so the header stays C++11-clean.
inline const char *magic() { return "RCSB1"; }
inline std::size_t magicLen() { return 5; }

// FNV-1a 64-bit, hex. Not cryptographic — only a content key for the
// shader cache (matches the rest of the codebase's hashing choice).
inline std::string fnv1a64Hex(const std::string &content)
{
    const uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
    const uint64_t FNV_PRIME  = 0x100000001b3ULL;
    uint64_t h = FNV_OFFSET;
    for (unsigned char c : content)
    {
        h ^= c;
        h *= FNV_PRIME;
    }
    static const char *hex = "0123456789abcdef";
    std::string out = "fnv1a64:";
    for (int shift = 60; shift >= 0; shift -= 4)
        out += hex[(h >> shift) & 0xF];
    return out;
}

// Content hash over the whole bundle: entries sorted by relPath, each
// contributing relPath + '\0' + data. Deterministic regardless of the
// order files were collected, so the same preset always hashes the same.
inline std::string hashEntries(std::vector<Entry> entries)
{
    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) { return a.relPath < b.relPath; });
    std::string acc;
    for (const auto &e : entries)
    {
        acc += e.relPath;
        acc.push_back('\0');
        acc += e.data;
    }
    return fnv1a64Hex(acc);
}

namespace detail
{
    inline void putU32(std::string &s, uint32_t v)
    {
        char b[4] = {char(v & 0xFF), char((v >> 8) & 0xFF),
                     char((v >> 16) & 0xFF), char((v >> 24) & 0xFF)};
        s.append(b, 4);
    }
    inline void putU64(std::string &s, uint64_t v)
    {
        char b[8];
        for (int i = 0; i < 8; ++i) b[i] = char((v >> (8 * i)) & 0xFF);
        s.append(b, 8);
    }
    inline bool getU32(const std::string &s, size_t &pos, uint32_t &out)
    {
        if (pos + 4 > s.size()) return false;
        out = uint32_t(uint8_t(s[pos])) | (uint32_t(uint8_t(s[pos + 1])) << 8) |
              (uint32_t(uint8_t(s[pos + 2])) << 16) | (uint32_t(uint8_t(s[pos + 3])) << 24);
        pos += 4;
        return true;
    }
    inline bool getU64(const std::string &s, size_t &pos, uint64_t &out)
    {
        if (pos + 8 > s.size()) return false;
        out = 0;
        for (int i = 0; i < 8; ++i) out |= uint64_t(uint8_t(s[pos + i])) << (8 * i);
        pos += 8;
        return true;
    }
} // namespace detail

inline std::string pack(const std::vector<Entry> &entries)
{
    std::string out(magic(), magicLen());
    for (const auto &e : entries)
    {
        detail::putU32(out, static_cast<uint32_t>(e.relPath.size()));
        out += e.relPath;
        detail::putU64(out, static_cast<uint64_t>(e.data.size()));
        out += e.data;
    }
    return out;
}

// Reject paths that could escape the extraction root (absolute, drive
// letters, or any '..' component). The bundle is auth-gated, but a client
// must never write outside its cache dir regardless.
inline bool relPathIsSafe(const std::string &p)
{
    if (p.empty() || p.size() > 1024) return false;
    if (p.front() == '/' || p.front() == '\\') return false;
    if (p.size() >= 2 && p[1] == ':') return false; // C:\...
    size_t start = 0;
    for (size_t i = 0; i <= p.size(); ++i)
    {
        if (i == p.size() || p[i] == '/' || p[i] == '\\')
        {
            std::string comp = p.substr(start, i - start);
            if (comp == "..") return false;
            start = i + 1;
        }
    }
    return true;
}

inline bool unpack(const std::string &blob, std::vector<Entry> &out)
{
    out.clear();
    if (blob.size() < magicLen() ||
        std::memcmp(blob.data(), magic(), magicLen()) != 0)
    {
        return false;
    }
    size_t pos = magicLen();
    while (pos < blob.size())
    {
        uint32_t pathLen = 0;
        if (!detail::getU32(blob, pos, pathLen)) return false;
        if (pos + pathLen > blob.size()) return false;
        Entry e;
        e.relPath = blob.substr(pos, pathLen);
        pos += pathLen;
        uint64_t dataLen = 0;
        if (!detail::getU64(blob, pos, dataLen)) return false;
        if (pos + dataLen > blob.size()) return false;
        e.data = blob.substr(pos, static_cast<size_t>(dataLen));
        pos += static_cast<size_t>(dataLen);
        if (!relPathIsSafe(e.relPath)) return false;
        out.push_back(std::move(e));
    }
    return true;
}

} // namespace shaderbundle
