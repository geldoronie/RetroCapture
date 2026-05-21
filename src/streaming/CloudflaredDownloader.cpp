#include "CloudflaredDownloader.h"

#include "../utils/FilesystemCompat.h"
#include "../utils/Logger.h"
#include "../utils/Paths.h"

extern "C" {
#include <libavformat/avio.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/sha.h>
#include <libavutil/mem.h>
}

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/stat.h>
#  include <unistd.h>
#endif

// ─────────────────────────────────────────────────────────────────────
// Pinned cloudflared release.
//
// To bump: pick the new tag from
//   https://github.com/cloudflare/cloudflared/releases
// download the three assets locally and `sha256sum` each, then update
// the three constants below. Nothing else in the project needs to
// change.
//
// Sha256 verification gates execution — a mismatch deletes the file
// and refuses to run. Without it the binary (which gets execvp'd
// against the user's network) would be trusted purely on GitHub TLS,
// and we want defence in depth.
// ─────────────────────────────────────────────────────────────────────

namespace
{
    constexpr const char *kPinnedVersion = "2026.5.0";

    struct ReleaseAsset
    {
        const char *filename;     // GitHub asset name
        const char *sha256Hex;    // expected hash, lowercase hex
        uint64_t    sizeBytes;    // for the UI progress bar fallback
    };

#if defined(_WIN32)
    constexpr ReleaseAsset kAsset = {
        "cloudflared-windows-amd64.exe",
        "f141cded099c239171ad2cea6fb5da0fdaa2bd36104c3074d883f9546519eba7",
        53886304ull,
    };
    constexpr bool kPlatformSupported = true;
#elif defined(__linux__) && defined(__aarch64__)
    constexpr ReleaseAsset kAsset = {
        "cloudflared-linux-arm64",
        "2dc0945345677d27de3ae390a31c3b168866b48766da5f4cfd3fc473ce572303",
        36711117ull,
    };
    constexpr bool kPlatformSupported = true;
#elif defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
    constexpr ReleaseAsset kAsset = {
        "cloudflared-linux-amd64",
        "0095e46fdc88855d801c4d304cb1f5dd4bd656116c47ab94c2ad0ae7cda1c7ec",
        39020697ull,
    };
    constexpr bool kPlatformSupported = true;
#else
    // ARM32 (Pi 3 32-bit), macOS, BSDs etc. cloudflared does publish
    // some of these (cloudflared-linux-arm, cloudflared-linux-armhf,
    // cloudflared-darwin-*), but they aren't part of RetroCapture's
    // supported targets for this phase; adding them is a separate
    // issue.
    constexpr ReleaseAsset kAsset = { nullptr, nullptr, 0 };
    constexpr bool kPlatformSupported = false;
#endif

    constexpr const char *kReleaseUrlPrefix =
        "https://github.com/cloudflare/cloudflared/releases/download/";

    // Single in-flight download at a time. Set when beginDownloadAsync
    // is called and reset by the worker thread on Ready / Failed.
    std::atomic<bool> g_downloadInFlight{false};

    std::mutex   g_overrideMu;
    std::string  g_cliOverride;

    // ─────────────────────────────────────────────────────────────
    // Small helpers
    // ─────────────────────────────────────────────────────────────

    bool isExecutableFile(const fs::path &p)
    {
        if (!fs::exists(p) || !fs::is_regular_file(p)) return false;
#ifdef _WIN32
        return true; // .exe extension on Windows; no separate +x bit
#else
        struct stat st{};
        if (::stat(p.string().c_str(), &st) != 0) return false;
        return (st.st_mode & S_IXUSR) != 0;
#endif
    }

    fs::path cacheDir()
    {
        fs::path dir = fs::path(Paths::getUserDataDir()) / "cloudflared";
        try { fs::create_directories(dir); } catch (...) { /* best effort */ }
        return dir;
    }

    std::string findOnPath()
    {
#ifdef _WIN32
        const char *envPath = ::getenv("PATH");
        const char  sep    = ';';
        const char *exe    = "cloudflared.exe";
#else
        const char *envPath = ::getenv("PATH");
        const char  sep    = ':';
        const char *exe    = "cloudflared";
#endif
        if (!envPath) return {};
        std::string p(envPath);
        size_t start = 0;
        while (start <= p.size())
        {
            size_t end = p.find(sep, start);
            std::string dir = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!dir.empty())
            {
                fs::path candidate = fs::path(dir) / exe;
                if (isExecutableFile(candidate))
                {
                    return candidate.string();
                }
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return {};
    }

    std::string sha256HexOfFile(const fs::path &path)
    {
        FILE *fp = ::fopen(path.string().c_str(), "rb");
        if (!fp) return {};

        struct AVSHA *ctx = av_sha_alloc();
        if (!ctx) { ::fclose(fp); return {}; }
        av_sha_init(ctx, 256);

        std::array<uint8_t, 64 * 1024> buf{};
        for (;;)
        {
            size_t n = ::fread(buf.data(), 1, buf.size(), fp);
            if (n == 0) break;
            av_sha_update(ctx, buf.data(), n);
        }
        ::fclose(fp);

        std::array<uint8_t, 32> digest{};
        av_sha_final(ctx, digest.data());
        av_free(ctx);

        static constexpr char kHex[] = "0123456789abcdef";
        std::string out(64, '0');
        for (size_t i = 0; i < digest.size(); ++i)
        {
            out[i * 2 + 0] = kHex[(digest[i] >> 4) & 0xF];
            out[i * 2 + 1] = kHex[(digest[i] >> 0) & 0xF];
        }
        return out;
    }

    bool makeExecutable(const fs::path &path)
    {
#ifdef _WIN32
        (void)path;
        return true;
#else
        struct stat st{};
        const std::string s = path.string();
        if (::stat(s.c_str(), &st) != 0) return false;
        mode_t mode = st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH;
        return ::chmod(s.c_str(), mode) == 0;
#endif
    }

    // Streams an HTTPS asset through FFmpeg's avio (so we don't have
    // to depend on libcurl just for one download) and writes it
    // straight to disk while running sha256 over the bytes. The
    // pinned hash is checked once at the end; on mismatch we delete
    // the temp file and report Failed.
    void downloadWorker(CloudflaredDownloader::ProgressCallback cb)
    {
        using namespace CloudflaredDownloader;

        auto fire = [&cb](Stage s, float p01, uint64_t done, uint64_t total,
                          std::string err = {})
        {
            if (!cb) return;
            Progress pr;
            pr.stage       = s;
            pr.progress01  = p01;
            pr.bytesDone   = done;
            pr.bytesTotal  = total;
            pr.error       = std::move(err);
            cb(pr);
        };

        fire(Stage::Connecting, 0.0f, 0, 0);

        const std::string url = std::string(kReleaseUrlPrefix) +
                                kPinnedVersion + "/" + kAsset.filename;
        LOG_INFO("CloudflaredDownloader: fetching " + url);

        avformat_network_init();

        AVDictionary *opts = nullptr;
        // GitHub release downloads redirect via 302 to S3-backed CDN.
        // follow_location is on by default in modern FFmpeg, but we
        // pin it explicitly so a downstream FFmpeg build with a
        // non-default doesn't quietly break us.
        av_dict_set_int(&opts, "follow_location", 1, 0);
        av_dict_set_int(&opts, "rw_timeout",
                        static_cast<int64_t>(60) * 1000 * 1000, 0); // 60 s in µs
        av_dict_set(&opts, "user_agent", "RetroCapture/0.7", 0);

        AVIOContext *io = nullptr;
        int rc = avio_open2(&io, url.c_str(), AVIO_FLAG_READ, nullptr, &opts);
        av_dict_free(&opts);
        if (rc < 0)
        {
            char errbuf[256] = {};
            av_strerror(rc, errbuf, sizeof(errbuf));
            std::string err = std::string("HTTPS open failed: ") + errbuf;
            LOG_ERROR("CloudflaredDownloader: " + err);
            fire(Stage::Failed, 0.0f, 0, 0, std::move(err));
            g_downloadInFlight.store(false);
            return;
        }

        // FFmpeg reports content-length via avio_size when available.
        int64_t reportedSize = avio_size(io);
        const uint64_t totalBytes = reportedSize > 0
            ? static_cast<uint64_t>(reportedSize)
            : kAsset.sizeBytes;

        const fs::path dir      = cacheDir();
        const fs::path tempPath = dir / (std::string(kAsset.filename) + ".part");
        const fs::path finalPath = dir / kAsset.filename;

        // Clean any stale .part from a previous aborted run.
        try { fs::remove(tempPath); } catch (...) { /* best effort */ }

        FILE *out = ::fopen(tempPath.string().c_str(), "wb");
        if (!out)
        {
            avio_closep(&io);
            std::string err = "Cannot open temp file for writing: " + tempPath.string();
            LOG_ERROR("CloudflaredDownloader: " + err);
            fire(Stage::Failed, 0.0f, 0, 0, std::move(err));
            g_downloadInFlight.store(false);
            return;
        }

        struct AVSHA *sha = av_sha_alloc();
        if (sha) av_sha_init(sha, 256);

        std::array<uint8_t, 64 * 1024> buf{};
        uint64_t bytesDone = 0;
        uint64_t lastFiredAt = 0;
        bool ioError = false;

        for (;;)
        {
            int n = avio_read(io, buf.data(), static_cast<int>(buf.size()));
            // avio_read signals end-of-stream either as 0 (older FFmpeg
            // semantics) or AVERROR_EOF (newer FFmpeg, current Ubuntu
            // 24.04 libavformat). Both are clean completions, not
            // errors — only a different negative code means the
            // connection actually dropped mid-transfer.
            if (n == 0 || n == AVERROR_EOF) break;
            if (n < 0)
            {
                char errbuf[256] = {};
                av_strerror(n, errbuf, sizeof(errbuf));
                LOG_ERROR(std::string("CloudflaredDownloader: read failed mid-download: ") + errbuf);
                ioError = true;
                break;
            }
            if (::fwrite(buf.data(), 1, static_cast<size_t>(n), out) != static_cast<size_t>(n))
            {
                LOG_ERROR("CloudflaredDownloader: short write to temp file");
                ioError = true;
                break;
            }
            if (sha) av_sha_update(sha, buf.data(), static_cast<unsigned int>(n));
            bytesDone += static_cast<uint64_t>(n);

            // Throttle progress callbacks to roughly one every 256 KB
            // so the UI thread doesn't drown in updates.
            if (bytesDone - lastFiredAt >= 256 * 1024 || bytesDone == totalBytes)
            {
                lastFiredAt = bytesDone;
                float p01 = (totalBytes > 0)
                    ? std::min(1.0f, static_cast<float>(bytesDone) / static_cast<float>(totalBytes))
                    : 0.0f;
                fire(Stage::Downloading, p01, bytesDone, totalBytes);
            }
        }

        ::fclose(out);
        avio_closep(&io);

        if (ioError)
        {
            if (sha) av_free(sha);
            try { fs::remove(tempPath); } catch (...) {}
            fire(Stage::Failed, 0.0f, bytesDone, totalBytes, "Network error during download");
            g_downloadInFlight.store(false);
            return;
        }

        fire(Stage::Verifying, 1.0f, bytesDone, totalBytes);

        // Finalize sha256.
        std::string actualHash;
        if (sha)
        {
            std::array<uint8_t, 32> digest{};
            av_sha_final(sha, digest.data());
            av_free(sha);
            static constexpr char kHex[] = "0123456789abcdef";
            actualHash.resize(64);
            for (size_t i = 0; i < digest.size(); ++i)
            {
                actualHash[i * 2 + 0] = kHex[(digest[i] >> 4) & 0xF];
                actualHash[i * 2 + 1] = kHex[(digest[i] >> 0) & 0xF];
            }
        }
        else
        {
            // Sha context allocation failed earlier (very unlikely);
            // fall back to a second pass over the file.
            actualHash = sha256HexOfFile(tempPath);
        }

        if (actualHash != kAsset.sha256Hex)
        {
            try { fs::remove(tempPath); } catch (...) {}
            std::string err = "SHA256 mismatch — expected " +
                              std::string(kAsset.sha256Hex) +
                              ", got " + actualHash +
                              ". Possible network corruption or compromised release.";
            LOG_ERROR("CloudflaredDownloader: " + err);
            fire(Stage::Failed, 1.0f, bytesDone, totalBytes, std::move(err));
            g_downloadInFlight.store(false);
            return;
        }

        fire(Stage::Installing, 1.0f, bytesDone, totalBytes);

        // Move temp → final. Both paths sit in <user-data-dir>/cloudflared/
        // so it's a same-filesystem rename — atomic on POSIX, and on
        // Windows MoveFile via the compat header. If a stale binary
        // exists from a previous version, drop it first so rename
        // doesn't fail on the cross-platform "target exists" rule.
        try { fs::remove(finalPath); } catch (...) { /* may not exist */ }
        try
        {
            fs::rename(tempPath, finalPath);
        }
        catch (const std::exception &ex)
        {
            std::string err = std::string("Failed to install binary to ") +
                              finalPath.string() + ": " + ex.what();
            LOG_ERROR("CloudflaredDownloader: " + err);
            try { fs::remove(tempPath); } catch (...) {}
            fire(Stage::Failed, 1.0f, bytesDone, totalBytes, std::move(err));
            g_downloadInFlight.store(false);
            return;
        }

        if (!makeExecutable(finalPath))
        {
            std::string err = "Failed to chmod +x on " + finalPath.string();
            LOG_ERROR("CloudflaredDownloader: " + err);
            // Don't bail — file is in place; many users will be able to
            // still use it (eg. invoked through an interpreter on
            // Windows). Report Failed anyway so the UI knows the next
            // spawn attempt will probably fail.
            fire(Stage::Failed, 1.0f, bytesDone, totalBytes, std::move(err));
            g_downloadInFlight.store(false);
            return;
        }

        LOG_INFO("CloudflaredDownloader: ready at " + finalPath.string() +
                 " (sha256 verified)");
        fire(Stage::Ready, 1.0f, bytesDone, totalBytes);
        g_downloadInFlight.store(false);
    }
}

namespace CloudflaredDownloader
{
    std::string cachedBinaryPath()
    {
        if (!kPlatformSupported) return {};
        return (cacheDir() / kAsset.filename).string();
    }

    bool isCached()
    {
        if (!kPlatformSupported) return false;
        return isExecutableFile(cachedBinaryPath());
    }

    void setCliOverride(const std::string &absolutePath)
    {
        std::lock_guard<std::mutex> lock(g_overrideMu);
        g_cliOverride = absolutePath;
    }

    std::string resolveBinaryPath()
    {
        // 1) CLI override wins outright.
        {
            std::lock_guard<std::mutex> lock(g_overrideMu);
            if (!g_cliOverride.empty()) return g_cliOverride;
        }
        // 2) Cached download.
        if (kPlatformSupported && isCached())
        {
            return cachedBinaryPath();
        }
        // 3) System PATH — preserves the pre-#53 behaviour for users
        //    who already had cloudflared installed.
        std::string p = findOnPath();
        if (!p.empty()) return p;
        return {};
    }

    bool isPlatformSupported() { return kPlatformSupported; }

    std::string pinnedVersion() { return kPinnedVersion; }

    bool beginDownloadAsync(ProgressCallback cb)
    {
        if (!kPlatformSupported)
        {
            if (cb)
            {
                Progress pr;
                pr.stage = Stage::Failed;
                pr.error = "No upstream cloudflared binary for this platform";
                cb(pr);
            }
            return false;
        }
        bool expected = false;
        if (!g_downloadInFlight.compare_exchange_strong(expected, true))
        {
            return false; // already running
        }
        std::thread([cb = std::move(cb)]() mutable {
            downloadWorker(std::move(cb));
        }).detach();
        return true;
    }
}
