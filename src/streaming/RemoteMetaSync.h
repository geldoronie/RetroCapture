#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief Polls the remote host's /meta endpoint and dispatches snapshot
 *        deltas via a callback.
 *
 * Phase 4 of issue #47. Runs in a background thread, fetches /meta every
 * `pollIntervalMs` (default ~1 s), parses the JSON snapshot defined in
 * docs/REMOTE_STREAM_PROTOCOL.md, and invokes the registered callback
 * whenever the host's shader / parameters change. Callback is invoked on
 * the polling thread — receivers must marshal back to the main / GL
 * thread if they touch GL state (ShaderEngine::loadPreset etc.).
 *
 * The polling cadence is a placeholder until Phase 6 wires a WebSocket
 * upgrade on the same endpoint for sub-second deltas.
 */
class RemoteMetaSync
{
public:
    struct ParamOverride
    {
        std::string name;
        float       value = 0.0f;
    };

    struct Snapshot
    {
        int                          protocolVersion = 0;
        std::string                  serverVersion;
        // shader block
        bool                         shaderActive    = false;
        bool                         pipelineEnabled = true;
        std::string                  preset;        // e.g. "crt/crt-mattias.glslp"
        std::string                  presetHash;
        std::vector<ParamOverride>   parameters;
        // source block
        uint32_t                     sourceWidth     = 0;
        uint32_t                     sourceHeight    = 0;
        uint32_t                     sourceFps       = 0;
        // image block — client uses these as seed values on the initial
        // connect (so the viewer starts with the same brightness /
        // aspect-ratio / output-size as the host). After the first
        // snapshot they're informational only; the user is free to
        // tweak the local Image tab without the next snapshot stomping
        // them.
        float                        imageBrightness     = 1.0f;
        float                        imageContrast       = 1.0f;
        bool                         imageMaintainAspect = true;
        uint32_t                     imageOutputWidth    = 0;
        uint32_t                     imageOutputHeight   = 0;
        // streaming block — for the OSD quick-actions widget (#68) to
        // surface "you're watching with N other viewers" in client
        // mode. Optional; older hosts that don't send this field
        // leave it at 0.
        uint32_t                     upstreamClientCount = 0;
        // chat block (#84) — the host's persistent chat-room slug
        // (UIManager::streamRoomSlug). Empty when the host has chat
        // turned off, or hasn't chosen a slug yet. Replaces the
        // earlier per-session chatStreamId, which produced one
        // orphan chat_rooms row per stream restart.
        std::string                  chatRoomSlug;
    };

    using SnapshotCallback = std::function<void(const Snapshot &)>;

    RemoteMetaSync();
    ~RemoteMetaSync();

    /**
     * @brief Start polling.
     * @param baseUrl  Remote server base URL (e.g. "http://host:8080").
     *                 "/meta" is appended internally.
     * @param cb       Invoked on every snapshot that differs from the last
     *                 one we delivered. Snapshots that match (same preset,
     *                 same hash, same parameter values) are filtered out.
     * @param pollIntervalMs Time between polls. Clamped to >= 250 ms.
     */
    bool start(const std::string &baseUrl, SnapshotCallback cb, int pollIntervalMs = 1000);

    /**
     * #49 Phase 3 — bearer token (sha256 hex of password) sent on
     * every /meta request. Must be called before start(). Empty
     * string disables the header. Mirrors VideoCaptureRemote's
     * setAuthToken.
     */
    void setAuthToken(const std::string &tokenHex) { m_authToken = tokenHex; }

    /**
     * @brief Stop the polling thread (joins).
     */
    void stop();

    bool isRunning() const { return m_running.load(); }

private:
    void pollLoop();

    /**
     * @brief Performs a single HTTP GET against `<baseUrl>/meta` and parses
     *        the JSON into `out`. Returns false on network / parse error.
     */
    bool fetchOnce(Snapshot &out);

    /**
     * @brief Opens an SSE long-poll on `<baseUrl>/meta` and dispatches
     *        snapshot deltas as they arrive. Returns true if the SSE
     *        handshake succeeded (the connection later ended cleanly or
     *        the server closed it), false if the server does not appear
     *        to support text/event-stream — the caller should fall back
     *        to short-poll fetchOnce() in that case. Phase 6 of #47.
     */
    bool runSSE();

    /**
     * @brief Common dispatch path — emits the callback if the snapshot
     *        differs from the last delivered one and updates the dedup
     *        state. Called from both polling and SSE paths.
     */
    void dispatchIfChanged(const Snapshot &snap);

    std::string         m_baseUrl;
    std::string         m_authToken;  // sha256 hex; empty == no auth
    SnapshotCallback    m_cb;
    int                 m_pollIntervalMs = 1000;

    std::thread         m_thread;
    std::atomic<bool>   m_running{false};
    // Live SSE socket fd, registered by runSSE() while it's blocked in a
    // no-timeout recv(). stop() shuts it down to break that recv at once
    // so the join() doesn't hang until the host's next event/keepalive
    // (which froze the UI on Disconnect). Stored as intptr_t so the
    // header doesn't pull in socket headers; -1 == none. #104.
    std::atomic<std::intptr_t> m_sseSock{-1};

    // Dedup state — only fire the callback when the snapshot actually
    // changed. Compared against the new snapshot before dispatch.
    std::string         m_lastPreset;
    std::string         m_lastPresetHash;
    bool                m_lastPipelineEnabled = true;
    std::vector<float>  m_lastParamValues;
    uint32_t            m_lastSourceWidth  = 0;
    uint32_t            m_lastSourceHeight = 0;
    uint32_t            m_lastSourceFps    = 0;
    uint32_t            m_lastUpstreamClientCount = 0;
};
