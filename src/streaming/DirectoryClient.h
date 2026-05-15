#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

/**
 * @brief Publishes (and continuously keeps alive) one stream entry in
 *        the public RetroCapture directory.
 *
 * Phase 2 of issue #49.
 *
 * Spawns a single background thread that drives this state machine:
 *
 *     Idle ──(start)──► Registering ──ok──► Active ──(stop)──► Stopping ──► Idle
 *                          │                  │                   │
 *                          │ failure          │ heartbeat fail    │ delete sent
 *                          ▼                  ▼                   ▼
 *                        Error            Error (retries)        Idle
 *
 * Idle:        no thread running, nothing published.
 * Registering: first request in flight; transient.
 * Active:      heartbeat every 30 s; PATCH on metadata change.
 * Error:       last attempt failed; retried with capped backoff.
 *              State persists so the UI can surface "publish broken".
 * Stopping:    DELETE in flight; thread will join.
 *
 * Failures NEVER bubble back into the local streaming pipeline. A
 * directory outage / network glitch should leave the user's stream
 * unaffected; the only visible effect is the publish state going
 * Error and the entry expiring after TTL on the server side.
 *
 * The lifecycle of streamId + ownerToken is "lifetime of this object's
 * Active run". On every `start()` we register fresh; we never
 * persist these across process restarts.
 */
class DirectoryClient
{
public:
    enum class State
    {
        Idle,
        Registering,
        Active,
        Stopping,
        Error,
    };

    /// Parameters required to register an entry.
    struct Config
    {
        std::string directoryUrl;         // base URL (e.g. "http://localhost:8081")
        std::string name;                 // 1..120 chars
        std::string hostNickname;         // 0..40 chars, optional
        std::string shader;               // current preset name
        uint32_t    resolutionW   = 0;
        uint32_t    resolutionH   = 0;
        uint32_t    fps           = 0;
        std::string codec         = "h264";        // "h264" | "h265"
        bool        passwordRequired = false;
        std::string endpoint;             // the URL clients should connect to
        std::string endpointMode  = "direct";      // "direct" | "tunnel-cloudflare" | "custom"
        std::string version       = "0.7.0-alpha"; // app version / protocol version string

        /// Validation called by start() — returns empty string on OK.
        std::string validate() const;
    };

    /// Subset of fields editable post-register via PATCH.
    struct Patch
    {
        std::string name;                 // empty = leave alone
        std::string hostNickname;
        std::string shader;
        uint32_t    resolutionW   = 0;    // 0 = leave alone
        uint32_t    resolutionH   = 0;
        uint32_t    fps           = 0;
        std::string codec;                // empty = leave alone
        bool        setPasswordRequired = false;
        bool        passwordRequired    = false;
        std::string endpoint;
        std::string endpointMode;
    };

    DirectoryClient();
    ~DirectoryClient();

    DirectoryClient(const DirectoryClient &)            = delete;
    DirectoryClient &operator=(const DirectoryClient &) = delete;

    /**
     * @brief Begin publishing. Spawns the worker thread. Returns false
     *        immediately if cfg is invalid; everything past that point
     *        is async.
     *
     * Calling start() while already Active is treated as "stop, then
     * register with the new config".
     */
    bool start(const Config &cfg);

    /// Schedule a delta. Will be sent on the next worker tick.
    void updateMetadata(const Patch &p);

    /// Report the current viewer count; folded into the next heartbeat.
    void setClientCount(int n);

    /// Stop publishing. Sends DELETE if we have a streamId, then joins.
    /// Idempotent. Safe to call from the destructor.
    void stop();

    State       getState() const     { return m_state.load(); }
    std::string getStreamId() const;
    std::string getLastError() const;

private:
    void workerLoop();
    bool doRegister();
    bool doHeartbeat();
    bool doPatchIfPending();
    bool doDelete();

    void setError(const std::string &msg);

    Config              m_cfg;            // guarded by m_mu while thread runs
    Patch               m_pendingPatch;   // guarded by m_mu
    bool                m_hasPendingPatch = false;
    int                 m_currentClientCount = 0;

    std::string         m_streamId;       // assigned by /register
    std::string         m_ownerToken;     // ditto
    std::string         m_lastError;

    mutable std::mutex  m_mu;
    std::condition_variable m_cv;         // wakes worker for early exit / patch

    std::thread         m_thread;
    std::atomic<bool>   m_stopRequested{false};
    std::atomic<State>  m_state{State::Idle};

    static constexpr std::chrono::seconds kHeartbeatInterval{30};
    static constexpr std::chrono::seconds kInitialBackoff{2};
    static constexpr std::chrono::seconds kMaxBackoff{60};
};
