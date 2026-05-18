#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

/**
 * @brief Spawns and supervises the `cloudflared` child process that
 *        exposes the local streaming endpoint as a public HTTPS URL.
 *
 * Phase 2.5 (partial) of issue #49.
 *
 * Scope of this first cut:
 *   - Quick Tunnel only (no Cloudflare account, random URL each run).
 *   - `cloudflared` assumed to be already installed and resolvable
 *     via PATH. Auto-download with SHA256 verification is deferred to
 *     a follow-up commit; for now the manager surfaces a clean
 *     "Error: cloudflared not found" state when the binary isn't
 *     available.
 *   - Linux x86_64 + Windows x86_64 build paths. Linux ARM64 should
 *     also work (cloudflared has an arm64 build) but isn't a primary
 *     test target.
 *
 * Lifecycle:
 *     start(port)  →  spawns `cloudflared tunnel --url http://localhost:<port>`
 *                     and starts a background thread reading stdout.
 *                     Returns immediately; getState()/getUrl() report
 *                     progress.
 *     getUrl()     →  empty until cloudflared prints its assigned URL,
 *                     then the trycloudflare.com hostname.
 *     stop()       →  terminates the child + joins the reader thread.
 *                     Idempotent.
 *
 * Failure handling: when the child exits unexpectedly the state goes
 * to Crashed and the URL is cleared. The caller (Application) decides
 * whether to restart; the manager itself does not auto-restart so a
 * misconfigured tunnel doesn't loop forever.
 */
class CloudflaredManager
{
public:
    enum class State
    {
        Idle,        // never started or fully stopped
        Spawning,    // child created, no URL yet
        Active,      // URL captured
        Stopping,    // stop() in flight
        NotFound,    // cloudflared binary missing
        Crashed,     // child exited without us asking it to
    };

    /**
     * Quick vs Named tunnel mode (#60).
     *
     *   Quick: `cloudflared tunnel --url http://localhost:<port>`.
     *          Cloudflare assigns a new trycloudflare.com URL each
     *          run; we parse it from stdout.
     *   Named: `cloudflared tunnel run --config <path> <id>`.
     *          The URL is fixed to the user's own hostname (already
     *          DNS-routed against the tunnel id), so there's nothing
     *          to parse from stdout — the URL is set up front by
     *          the caller via TunnelConfig::publicUrl.
     */
    enum class Mode
    {
        Quick,
        Named,
    };

    /**
     * Caller-supplied parameters for start(). Backwards-compat
     * convenience overload below preserves the old (port-only) API.
     */
    struct TunnelConfig
    {
        Mode        mode      = Mode::Quick;
        uint16_t    localPort = 0;
        std::string tunnelId;   // Named only — cloudflared tunnel id
        std::string publicUrl;  // Named only — pre-known hostname
    };

    CloudflaredManager();
    ~CloudflaredManager();

    CloudflaredManager(const CloudflaredManager &)            = delete;
    CloudflaredManager &operator=(const CloudflaredManager &) = delete;

    /**
     * @brief Spawn cloudflared in the requested mode.
     *
     * For Quick mode this is `cloudflared tunnel --url http://localhost:<port>`
     * and the URL appears asynchronously once cloudflared finishes
     * negotiating with the Cloudflare edge.
     *
     * For Named mode this is `cloudflared tunnel run --config <generated>.yml <id>`
     * where the generated YAML points the tunnel at localhost:<port>.
     * The URL is set on start (already known — it's the user's
     * configured hostname), so getUrl() returns it immediately.
     *
     * Returns false synchronously if the binary couldn't be invoked
     * at all (NotFound state); on platform-level success the state
     * machine takes over.
     */
    bool start(const TunnelConfig &cfg);

    /// Backwards-compat overload for the Quick-only call site that
    /// existed before #60. Equivalent to start({Quick, port}).
    bool start(uint16_t localPort);

    /**
     * @brief Terminate the child and join the reader thread.
     *        Idempotent — safe to call from the destructor.
     */
    void stop();

    /// Current high-level state.
    State getState() const { return m_state.load(); }

    /// The public tunnel URL once cloudflared has reported one.
    /// Empty until then.
    std::string getUrl() const;

    /// Best-effort failure message for the UI (non-localised).
    std::string getLastError() const;

private:
    void readerLoop();
    void terminateChild();

    std::string m_url;
    std::string m_lastError;
    mutable std::mutex m_mu;

    std::atomic<State> m_state{State::Idle};
    std::atomic<bool>  m_stopRequested{false};

    std::thread m_thread;

    // Platform-specific child handles. Type-erased via void* to keep
    // the header free of winsock / signal.h pollution.
#ifdef _WIN32
    void *m_processHandle = nullptr; // HANDLE
    void *m_stdoutHandle  = nullptr; // HANDLE (read end of stdout pipe)
#else
    int   m_pid           = -1;
    int   m_stdoutFd      = -1;
#endif
};
