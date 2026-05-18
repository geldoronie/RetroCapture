#pragma once

#include <cstdint>
#include <functional>
#include <string>

/**
 * @brief Locate (or fetch) the `cloudflared` binary used by
 *        CloudflaredManager to bring up a Quick Tunnel.
 *
 * Phase 2.5b of issue #49 (tracked as #53). Phase 2.5 shipped the
 * supervision layer but required the user to install cloudflared by
 * hand on PATH; this module makes the first-time experience
 * zero-friction:
 *
 *     1) If the user passed `--cloudflared-binary /path/to/x` on the
 *        CLI, use that verbatim. No download, no checksum (the user
 *        explicitly seeded it).
 *     2) Else if a previously-downloaded copy exists at
 *        `<user-data-dir>/cloudflared/cloudflared[.exe]`, use that.
 *     3) Else if `cloudflared` is on PATH, use that — preserves
 *        compatibility with users who already had it installed.
 *     4) Otherwise, the manager surfaces NotFound and the UI prompts
 *        the user to accept a download via `beginDownloadAsync`.
 *
 * The pinned release version + per-arch sha256 hashes live as
 * constants in the .cpp so a bump is a single PR touching one file.
 * The binary's bytes are sha256-checked against the pinned hash
 * before being moved into the final cache path; a mismatch deletes
 * the temp file and reports a clear error.
 *
 * No state is held by this class — all entry points are free
 * functions in the `CloudflaredDownloader` namespace.
 */
namespace CloudflaredDownloader
{
    /// Per-step status surfaced to the UI during an async download.
    enum class Stage
    {
        Connecting,   // resolving DNS + opening the HTTPS connection
        Downloading,  // bytes flowing; `progress01` valid in [0..1]
        Verifying,    // computing sha256, comparing against pinned
        Installing,   // atomic rename + chmod (Linux)
        Ready,        // binary is in place, resolveBinaryPath() works
        Failed,       // see `error` for a non-localised explanation
    };

    /// Snapshot fed to the progress callback on every update.
    struct Progress
    {
        Stage       stage    = Stage::Connecting;
        float       progress01 = 0.0f;   // valid only when stage == Downloading
        uint64_t    bytesDone   = 0;
        uint64_t    bytesTotal  = 0;     // 0 when content-length is unknown
        std::string error;               // populated only on Stage::Failed
    };

    using ProgressCallback = std::function<void(const Progress &)>;

    /**
     * @brief Returns the path that should be passed to execvp /
     *        CreateProcess. Empty string means "no usable binary
     *        found" — the UI is expected to offer a download in
     *        that case.
     */
    std::string resolveBinaryPath();

    /**
     * @brief Path under the user-data dir where the downloaded
     *        binary lives (regardless of whether it currently exists).
     *        Always returns a value, even on Idle/Failed states, so
     *        the UI can show "we would put it at …".
     */
    std::string cachedBinaryPath();

    /**
     * @brief Convenience over resolveBinaryPath() — true iff
     *        cachedBinaryPath() exists and is executable.
     */
    bool isCached();

    /**
     * @brief Override hook for the `--cloudflared-binary` CLI flag.
     *        Setting a non-empty path makes resolveBinaryPath() return
     *        it directly, bypassing the cache and PATH lookup, and
     *        suppresses any download prompt. Passing an empty string
     *        clears the override.
     */
    void setCliOverride(const std::string &absolutePath);

    /**
     * @brief Kick off the download on a detached background thread.
     *        Fires `cb` with successive Progress values on that same
     *        thread until Ready or Failed. The caller owns `cb`'s
     *        captures — keep them alive for the lifetime of the
     *        download (typically the UI window stays open).
     *
     *        Returns false synchronously if a download was already in
     *        flight (no-op) or if the current platform isn't supported
     *        upstream by cloudflared (ARM32, MacOS — those should not
     *        even surface the Cloudflare Tunnel option in the UI, this
     *        is defense in depth).
     */
    bool beginDownloadAsync(ProgressCallback cb);

    /**
     * @brief True iff the current build target has an upstream
     *        cloudflared release. ARM32 has no first-party binary, so
     *        the UI hides the Cloudflare Tunnel option entirely.
     */
    bool isPlatformSupported();

    /// Pinned cloudflared release version, for UI display.
    std::string pinnedVersion();
}
