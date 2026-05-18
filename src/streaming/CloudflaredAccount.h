#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

/**
 * @brief One-shot, side-effecting cloudflared invocations that aren't
 *        the long-running tunnel supervision handled by CloudflaredManager.
 *
 * Phase 2.5c of issue #49 (tracked as #60). Named Tunnel support
 * needs the user to authenticate against their own Cloudflare
 * account, pick or create a tunnel, and bind a DNS record to it.
 * Each of those is a discrete `cloudflared tunnel <subcommand>`
 * call that finishes in seconds (or minutes, for the OAuth wait) —
 * different lifecycle from the persistent tunnel process. Keeping
 * them in their own namespace avoids inflating CloudflaredManager's
 * already crowded supervision logic.
 *
 * All subprocesses use the binary resolved by
 * CloudflaredDownloader::resolveBinaryPath(); a missing binary is
 * surfaced as a clear error from each call.
 */
namespace CloudflaredAccount
{
    /// True iff the user has already run `cloudflared tunnel login`
    /// and the resulting cert.pem is on disk.
    /// Path is ~/.cloudflared/cert.pem on Linux,
    /// %USERPROFILE%\.cloudflared\cert.pem on Windows.
    bool hasCredentials();

    /// Compact view of a tunnel as reported by
    /// `cloudflared tunnel list --output json`.
    struct TunnelInfo
    {
        std::string id;        // UUID
        std::string name;      // user-supplied label
        std::string createdAt; // RFC3339
    };

    /// Progress snapshot for an in-flight `cloudflared tunnel login`.
    /// Login is async because the OAuth round-trip waits on the user
    /// clicking through the browser dialog, which can take minutes.
    struct LoginProgress
    {
        enum class Stage
        {
            Starting,       // child spawned, waiting for OAuth URL
            AwaitingAuth,   // OAuth URL captured; cert.pem not on disk yet
            Complete,       // cert.pem exists
            Failed,         // see `error`
            Cancelled,      // cancelLogin() was called
        };
        Stage       stage    = Stage::Starting;
        std::string oauthUrl;  // populated once parsed from stdout
        std::string error;     // populated on Failed
    };
    using LoginCallback = std::function<void(const LoginProgress &)>;

    /// Spawn `cloudflared tunnel login` on a detached thread. Fires
    /// `cb` with successive LoginProgress values until terminal
    /// state. The OAuth URL is parsed from the child's stdout and
    /// surfaced via LoginProgress::oauthUrl so a headless / SSH user
    /// can copy-paste it into a browser on another machine.
    ///
    /// Returns false if a login was already running (no-op).
    bool beginLoginAsync(LoginCallback cb);

    /// Best-effort cancel of an in-flight login. Sends SIGTERM /
    /// TerminateProcess to the child. The callback fires once more
    /// with Stage::Cancelled.
    void cancelLogin();

    /// True if a login is currently running.
    bool isLoginInFlight();

    /// `cloudflared tunnel list --output json`. Synchronous; ~10 s
    /// timeout. On error returns an empty vector and populates
    /// `errorOut`. Caller decides whether to surface the error to
    /// the UI or to silently render an empty list.
    std::vector<TunnelInfo> listTunnels(std::string &errorOut);

    /// `cloudflared tunnel create <name>`. Returns the new tunnel id
    /// on success, empty string + populated `errorOut` on failure.
    /// Name is what the user types into the "Create new..." dialog.
    std::string createTunnel(const std::string &name, std::string &errorOut);

    /// `cloudflared tunnel route dns <id> <hostname>`. Hostname must
    /// be on a Cloudflare-managed zone the user controls; the
    /// command fails with a clear message otherwise. Returns false +
    /// populated `errorOut` on failure.
    bool routeDns(const std::string &tunnelId,
                  const std::string &hostname,
                  std::string       &errorOut);
}
