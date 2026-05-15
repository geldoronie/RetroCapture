#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief Polls `GET /streams` on the directory service and exposes a
 *        thread-safe snapshot of the result to the UI.
 *
 * Phase 4 of issue #49.
 *
 * Lifecycle:
 *   - start(directoryUrl) → spawns the worker thread
 *   - stop() → joins, idempotent
 *   - refreshNow() → asks the worker to fetch immediately
 *
 * The worker re-fetches every kPollInterval (30 s by default) so the
 * browse list reflects new / departed entries without the user having
 * to click refresh. A manual refreshNow() short-circuits the wait.
 *
 * Fetch failures are non-fatal: the worker logs a warning, leaves the
 * previous snapshot intact (so transient outages don't blank the
 * list), and tries again on the next tick.
 */
class DirectoryBrowser
{
public:
    /// One entry as the UI sees it. Mirrors the wire fields in
    /// docs/DIRECTORY_PROTOCOL.md; we omit anything we don't need to
    /// render (publicIp, registeredAt, lastHeartbeatAt) to keep the
    /// memory footprint small even when listing hundreds of streams.
    struct Entry
    {
        std::string streamId;
        std::string name;
        std::string hostNickname;
        std::string shader;
        uint32_t    resolutionW   = 0;
        uint32_t    resolutionH   = 0;
        uint32_t    fps           = 0;
        std::string codec;
        bool        passwordRequired = false;
        std::string endpoint;
        std::string endpointMode;    // "direct" | "tunnel-cloudflare" | "custom"
        int         clientCount   = 0;
        std::string version;
        std::string expiresAt;       // RFC 3339, for the "fresh?" indicator
    };

    /// Snapshot returned by getSnapshot(). The UI takes a copy each
    /// frame; it's small enough that the copy is cheap.
    struct Snapshot
    {
        std::vector<Entry>           entries;
        int                          totalCount = 0;    // pre-limit total reported by the server
        std::chrono::steady_clock::time_point fetchedAt; // local clock of the last successful fetch
        std::string                  lastError;          // non-empty when the most recent fetch failed
    };

    DirectoryBrowser();
    ~DirectoryBrowser();

    DirectoryBrowser(const DirectoryBrowser &)            = delete;
    DirectoryBrowser &operator=(const DirectoryBrowser &) = delete;

    /// Begin polling. If already running with a different URL, stops
    /// and restarts. Returns false on validation failure.
    bool start(const std::string &directoryUrl);

    /// Wake the worker to fetch right now instead of waiting for the
    /// next poll tick. Safe to call from any thread, no-op when idle.
    void refreshNow();

    /// Stop polling. Idempotent.
    void stop();

    bool isRunning() const { return m_running.load(); }

    /// Returns a copy of the latest snapshot.
    Snapshot getSnapshot() const;

private:
    void workerLoop();
    bool fetchOnce(Snapshot &out);

    std::string m_directoryUrl;
    mutable std::mutex m_snapshotMu;
    Snapshot m_snapshot;

    std::mutex m_wakeMu;
    std::condition_variable m_wakeCv;
    bool m_refreshRequested = false;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};

    static constexpr std::chrono::seconds kPollInterval{30};
};
