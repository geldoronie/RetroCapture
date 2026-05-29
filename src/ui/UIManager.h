#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <cstring>
#include <memory>
#include "../renderer/glad_loader.h"
#include "../capture/IVideoCapture.h"

struct GLFWwindow;
#ifdef USE_SDL2
struct SDL_Window;
#endif

class IVideoCapture;
class ShaderEngine;
class IAudioCapture;

// Forward declarations
class UIConfigurationSource;
class UIConfigurationShader;
class UIConfigurationImage;
class UIConfigurationStreaming;
class UIConfigurationRecording;
class UIConfigurationWebPortal;
class UIConfigurationAudio;
class UIConfigurationVirtualCamera;
class UIInfoPanel;
class QuickActionsOverlay;
class UIPreferences;
class UICredits;
class UIRemoteConnection;
class UIDirectoryBrowser;
class UICapturePresets;
class UIRecordings;
class RecordingProfileManager;
class StreamingProfileManager;

class UIManager
{
public:
    UIManager();
    ~UIManager();

    bool init(void *window); // Accepts GLFWwindow* or SDL_Window*
    void shutdown();

    void beginFrame();
    void endFrame();

    void render();

    /// Always-on-top corner overlay that surfaces remote-connection
    /// state transitions (Connecting / Reconnecting / Disconnecting
    /// / Connected). Rendered before render()'s F12-visibility gate
    /// so the user sees connection feedback even with the rest of
    /// the IMGUI surface hidden. Delegates to
    /// osd::ConnectionStatusOverlay since #68.
    void renderConnectionOverlay();

    // Callbacks para interação
    void setShaderList(const std::vector<std::string> &shaders) { m_shaderList = shaders; }
    void setCurrentShader(const std::string &shader)
    {
        m_currentShader = shader;
        if (m_onShaderChanged)
        {
            m_onShaderChanged(shader);
        }
    }
    void setOnShaderChanged(std::function<void(const std::string &)> callback) { m_onShaderChanged = callback; }

    // Parâmetros de shader
    void setShaderEngine(ShaderEngine *engine) { m_shaderEngine = engine; }
    ShaderEngine *getShaderEngine() const { return m_shaderEngine; }
    
    // Get preset window for Application connection
    UICapturePresets* getCapturePresetsWindow() { return m_capturePresetsWindow.get(); }
    
    // Get recordings window for Application connection
    UIRecordings* getRecordingsWindow() { return m_recordingsWindow.get(); }
    void setOnSavePreset(std::function<void(const std::string &, bool)> callback) { m_onSavePreset = callback; }
    const std::function<void(const std::string &, bool)> &getOnSavePreset() const { return m_onSavePreset; }
    const std::vector<std::string> &getScannedShaders() const { return m_scannedShaders; }

    void setBrightness(float brightness)
    {
        m_brightness = brightness;
        if (m_onBrightnessChanged)
        {
            m_onBrightnessChanged(brightness);
        }
    }
    void setContrast(float contrast)
    {
        m_contrast = contrast;
        if (m_onContrastChanged)
        {
            m_onContrastChanged(contrast);
        }
    }
    void setOnBrightnessChanged(std::function<void(float)> callback) { m_onBrightnessChanged = callback; }
    void setOnContrastChanged(std::function<void(float)> callback) { m_onContrastChanged = callback; }

    void setMaintainAspect(bool maintain)
    {
        m_maintainAspect = maintain;
        if (m_onMaintainAspectChanged)
        {
            m_onMaintainAspectChanged(maintain);
        }
        saveConfig(); // Salvar configuração quando mudar (via API ou UI)
    }
    void setOnMaintainAspectChanged(std::function<void(bool)> callback) { m_onMaintainAspectChanged = callback; }

    void setFullscreen(bool fullscreen)
    {
        m_fullscreen = fullscreen;
        if (m_onFullscreenChanged)
        {
            m_onFullscreenChanged(fullscreen);
        }
    }
    void setOnFullscreenChanged(std::function<void(bool)> callback) { m_onFullscreenChanged = callback; }
    void setMonitorIndex(int index)
    {
        m_monitorIndex = index;
        if (m_onMonitorIndexChanged)
        {
            m_onMonitorIndexChanged(index);
        }
    }
    void setOnMonitorIndexChanged(std::function<void(int)> callback) { m_onMonitorIndexChanged = callback; }
    
    // Resolução de saída configurável
    void setOutputResolution(uint32_t width, uint32_t height)
    {
        m_outputWidth = width;
        m_outputHeight = height;
        if (m_onOutputResolutionChanged)
        {
            m_onOutputResolutionChanged(width, height);
        }
    }
    void setOnOutputResolutionChanged(std::function<void(uint32_t, uint32_t)> callback) { m_onOutputResolutionChanged = callback; }
    uint32_t getOutputWidth() const { return m_outputWidth; }
    uint32_t getOutputHeight() const { return m_outputHeight; }

    // Controles V4L2
    void setCaptureControls(IVideoCapture *capture); // Genérico para V4L2 e DirectShow
    // Deprecated: use setCaptureControls instead
    void setV4L2Controls(IVideoCapture *capture) { setCaptureControls(capture); }
    void setOnV4L2ControlChanged(std::function<void(const std::string &, int32_t)> callback)
    {
        m_onV4L2ControlChanged = callback;
    }
    void setOnDeviceChanged(std::function<void(const std::string &)> callback) { m_onDeviceChanged = callback; }

    // Source type enum (declared early for use in callbacks)
    enum class SourceType
    {
        None = 0,
        V4L2 = 1,
        DS = 2,           // DirectShow (Windows)
        Remote = 3,       // Remote /raw MPEG-TS from another RetroCapture (Phase 3 of #47)
        AVFoundation = 4  // AVFoundation (macOS)
    };

    // Source type setter (para uso pelas classes de abas)
    void setSourceType(SourceType sourceType);

    void setOnSourceTypeChanged(std::function<void(SourceType)> callback) { m_onSourceTypeChanged = callback; }
    SourceType getSourceType() const { return m_sourceType; }
    // Phase 5 of #47: client mode (consuming a remote /raw) — UI controls
    // should render disabled, and a banner advertises the connection.
    bool isRemoteSource() const { return m_sourceType == SourceType::Remote; }
    void setCurrentDevice(const std::string &device)
    {
        m_currentDevice = device;
        if (m_onDeviceChanged)
        {
            m_onDeviceChanged(device);
        }
    }
    // Update the device path without firing the device-change callback.
    // Used by Application when it needs to mark a failed connect cleared
    // from inside the same callback — calling setCurrentDevice would
    // re-enter and either hang or recurse depending on the guard.
    void setCurrentDeviceSilent(const std::string &device)
    {
        m_currentDevice = device;
    }
    std::string getCurrentDevice() const { return m_currentDevice; }

    // Métodos auxiliares para disparar callbacks via API (tornados públicos para uso pelas classes de abas)
    void triggerSourceTypeChange(SourceType sourceType)
    {
        m_sourceType = sourceType;
        if (m_onSourceTypeChanged)
        {
            m_onSourceTypeChanged(sourceType);
        }
    }

    void triggerResolutionChange(uint32_t width, uint32_t height)
    {
        m_captureWidth = width;
        m_captureHeight = height;
        if (m_onResolutionChanged)
        {
            m_onResolutionChanged(width, height);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    void triggerFramerateChange(uint32_t fps)
    {
        m_captureFps = fps;
        if (m_onFramerateChanged)
        {
            m_onFramerateChanged(fps);
        }
        saveConfig(); // Salvar configuração quando mudar
    }

    void triggerV4L2ControlChange(const std::string &name, int32_t value)
    {
        if (m_onV4L2ControlChanged)
        {
            m_onV4L2ControlChanged(name, value);
        }
    }

    void triggerDeviceChange(const std::string &device);

    // V4L2Control struct - public for API access
    struct V4L2Control
    {
        std::string name;
        int32_t value;
        int32_t min;
        int32_t max;
        int32_t step;
        bool available;
    };

    void refreshV4L2Devices();

    // Informações da captura
    void setCaptureInfo(uint32_t width, uint32_t height, uint32_t fps, const std::string &device);
    void setOnResolutionChanged(std::function<void(uint32_t, uint32_t)> callback) { m_onResolutionChanged = callback; }
    void setOnFramerateChanged(std::function<void(uint32_t)> callback) { m_onFramerateChanged = callback; }

    // Visibilidade da UI
    bool isVisible() const { return m_uiVisible; }
    void setVisible(bool visible);
    void toggle() { setVisible(!m_uiVisible); }
    void setOnVisibilityChanged(std::function<void(bool)> callback) { m_onVisibilityChanged = callback; }

    // Streaming info setters (public)
    void setStreamingActive(bool active) { m_streamingActive = active; }
    void setStreamUrl(const std::string &url) { m_streamUrl = url; }
    void setStreamClientCount(uint32_t count) { m_streamClientCount = count; }
    void setCanStartStreaming(bool canStart) { m_canStartStreaming = canStart; }
    void setStreamingCooldownRemainingMs(int64_t ms) { m_streamingCooldownRemainingMs = ms; }
    void setStreamingProcessing(bool processing) { m_streamingProcessing = processing; }
    bool isStreamingProcessing() const { return m_streamingProcessing; }

    // ── Public-directory publish settings (#49 Phase 2) ──
    bool getDirectoryPublishEnabled() const          { return m_directoryPublishEnabled; }
    void setDirectoryPublishEnabled(bool v)          { m_directoryPublishEnabled = v; }
    const std::string &getDirectoryUrl() const       { return m_directoryUrl; }
    void setDirectoryUrl(const std::string &v)       { m_directoryUrl = v; }
    bool getDirectoryInsecureSkipVerify() const      { return m_directoryInsecureSkipVerify; }
    void setDirectoryInsecureSkipVerify(bool v)      { m_directoryInsecureSkipVerify = v; }
    // #84 — Chat service base URL (ws:// or wss://). Editable under
    // Configurations → Streaming → Advanced, mirroring the directory
    // URL field. Application reads this every frame and reconfigures
    // the ChatClient when it changes (cheap no-op when unchanged).
    const std::string &getChatBaseUrl() const        { return m_chatBaseUrl; }
    void setChatBaseUrl(const std::string &v)        { m_chatBaseUrl = v; }
    // #84 — Persistent chat nickname. Shared between host and viewer
    // modes (you're "geldo" regardless of which side you're on); the
    // OSD chat panel writes here when the user clicks Apply. Empty
    // means "use a fallback": Application falls back to the directory
    // host nickname when publishing, or sends an empty hello (server
    // generates anon-<rand>) when viewing.
    const std::string &getChatNickname() const       { return m_chatNickname; }
    void setChatNickname(const std::string &v)       { m_chatNickname = v; }
    // #84 — Cross-window request to open the Chat Profile dialog.
    // Set by UIConfigurationStreaming when the user clicks
    // "Configure Profile"; consumed by OSDChat on the next frame.
    // The flag is a one-shot — the consumer clears it so a stale
    // request from minutes ago doesn't pop the dialog every frame.
    bool consumeOpenChatProfileRequest()
    {
        if (!m_openChatProfileRequested) return false;
        m_openChatProfileRequested = false;
        return true;
    }
    void requestOpenChatProfile()                    { m_openChatProfileRequested = true; }
    // #84 — Same one-shot pattern for the Chat Rooms window. Set by
    // the QuickActions overlay; consumed by OSDChat on the next frame
    // (also forces the chat panel itself visible if it was hidden).
    bool consumeOpenChatRoomsRequest()
    {
        if (!m_openChatRoomsRequested) return false;
        m_openChatRoomsRequested = false;
        return true;
    }
    void requestOpenChatRooms()                      { m_openChatRoomsRequested = true; }
    // #84 — "Open the chat alongside the stream" master toggle. When
    // off, Application skips chat provisioning entirely; /meta omits
    // chat.roomSlug so viewers don't auto-bind. Default ON so the
    // out-of-the-box experience matches the prior auto-create-room
    // behaviour. Editable under Streaming → Public Directory.
    bool getStreamChatEnabled() const                { return m_streamChatEnabled; }
    void setStreamChatEnabled(bool v)                { m_streamChatEnabled = v; }
    // #84 — Human-readable name of the streamer's chat room. User-
    // editable in Streaming → Chat room. Empty == "use default
    // 'Stream of <nick>' at provision time".
    const std::string &getStreamRoomTitle() const    { return m_streamRoomTitle; }
    void setStreamRoomTitle(const std::string &v)    { m_streamRoomTitle = v; }
    // #84 — Auto-derived public slug for the streamer's room. Not
    // user-editable; computed from the room title (or the fallback
    // "Stream of <nick>") at first stream start, persisted here so
    // every reconnect lands on the same room. Empty == "not yet
    // provisioned"; Application fills it on the first stream start
    // with chat enabled.
    const std::string &getStreamRoomSlug() const     { return m_streamRoomSlug; }
    void setStreamRoomSlug(const std::string &v)     { m_streamRoomSlug = v; }

    // #85 — Virtual camera (Linux v4l2loopback in Phase 1). Config
    // round-trips through streaming.virtcam in retrocapture.conf.
    // Runtime state (status / error text) lives here too, written
    // by Application after each syncVirtualCamera tick so the
    // configuration window can render a live status line.
    bool getVirtcamEnabled() const                   { return m_virtcamEnabled; }
    void setVirtcamEnabled(bool v)                   { m_virtcamEnabled = v; }
    const std::string &getVirtcamDevicePath() const  { return m_virtcamDevicePath; }
    void setVirtcamDevicePath(const std::string &v)  { m_virtcamDevicePath = v; }
    uint32_t getVirtcamOutputWidth() const           { return m_virtcamOutputWidth; }
    void setVirtcamOutputWidth(uint32_t v)           { m_virtcamOutputWidth = v; }
    uint32_t getVirtcamOutputHeight() const          { return m_virtcamOutputHeight; }
    void setVirtcamOutputHeight(uint32_t v)          { m_virtcamOutputHeight = v; }
    uint32_t getVirtcamOutputFps() const             { return m_virtcamOutputFps; }
    void setVirtcamOutputFps(uint32_t v)             { m_virtcamOutputFps = v; }
    const std::string &getVirtcamPixelFormat() const { return m_virtcamPixelFormat; }
    void setVirtcamPixelFormat(const std::string &v) { m_virtcamPixelFormat = v; }
    // Status surface (written by Application; read by UI).
    const std::string &getVirtcamStatusText() const  { return m_virtcamStatusText; }
    void setVirtcamStatusText(const std::string &v)  { m_virtcamStatusText = v; }
    const std::string &getVirtcamErrorText() const   { return m_virtcamErrorText; }
    void setVirtcamErrorText(const std::string &v)   { m_virtcamErrorText = v; }
    // #85 — Synchronous handshake for "stop the sink right now so
    // I can `rmmod` the kernel module". UI sets requestVirtcamStop,
    // Application::syncVirtualCamera consumes it on the next render
    // frame: forces m_virtcam->stop() AND sets virtcamStoppedNotice
    // so the UI worker can poll until it's safe to fire pkexec
    // rmmod. Two atomic-ish bools (bool is fine here — UI thread
    // and render thread is the same thread; the worker reads the
    // *Notice flag but only after spinning a sleep, no race).
    bool consumeVirtcamStopRequest()
    {
        if (!m_virtcamStopRequested) return false;
        m_virtcamStopRequested = false;
        return true;
    }
    void requestVirtcamStop()                        { m_virtcamStopRequested = true; }
    bool isVirtcamStopped() const                    { return m_virtcamStoppedNotice; }
    void setVirtcamStopped(bool v)                   { m_virtcamStoppedNotice = v; }
    // Preferences (#45 placeholder + window restructure). Persisted
    // today; the TranslationManager that consumes the language code
    // lands in Fase B.
    const std::string &getLanguage() const           { return m_language; }
    void setLanguage(const std::string &v)           { m_language = v; }
    bool getStartFullscreen() const                  { return m_startFullscreen; }
    void setStartFullscreen(bool v)                  { m_startFullscreen = v; }
    const std::string &getDirectoryStreamName() const { return m_directoryStreamName; }
    void setDirectoryStreamName(const std::string &v) { m_directoryStreamName = v; }
    // #84 — As of the profile unification, the directory's "host
    // nickname" is derived from the chat Profile (m_chatNickname).
    // The setter is still here for ConfigJSON round-trips of legacy
    // configs that have a separate hostNickname field; on first
    // load we mirror it into m_chatNickname when the latter is
    // empty, then the chat profile owns the value going forward.
    const std::string &getDirectoryHostNickname() const
    {
        return m_chatNickname.empty() ? m_directoryHostNickname : m_chatNickname;
    }
    void setDirectoryHostNickname(const std::string &v) { m_directoryHostNickname = v; }
    const std::string &getDirectoryPassword() const  { return m_directoryPassword; }
    void setDirectoryPassword(const std::string &v)  { m_directoryPassword = v; }
    const std::string &getDirectoryEndpointMode() const { return m_directoryEndpointMode; }
    void setDirectoryEndpointMode(const std::string &v) { m_directoryEndpointMode = v; }
    const std::string &getDirectoryCustomEndpoint() const { return m_directoryCustomEndpoint; }
    void setDirectoryCustomEndpoint(const std::string &v) { m_directoryCustomEndpoint = v; }
    // Phase 2.5c (#60): Cloudflare Tunnel sub-mode + Named-tunnel state.
    // tunnelMode is "quick" (default) or "named". When "named", the
    // tunnel id + hostname identify which existing tunnel cloudflared
    // should run, and the directory entry's URL points at the user's
    // own hostname instead of a fresh trycloudflare.com one.
    const std::string &getDirectoryTunnelMode() const { return m_directoryTunnelMode; }
    void setDirectoryTunnelMode(const std::string &v) { m_directoryTunnelMode = v; }
    const std::string &getDirectoryNamedTunnelId() const { return m_directoryNamedTunnelId; }
    void setDirectoryNamedTunnelId(const std::string &v) { m_directoryNamedTunnelId = v; }
    const std::string &getDirectoryNamedTunnelHostname() const { return m_directoryNamedTunnelHostname; }
    void setDirectoryNamedTunnelHostname(const std::string &v) { m_directoryNamedTunnelHostname = v; }
    bool getDirectoryPrivacyAcked() const            { return m_directoryPrivacyAcked; }
    void setDirectoryPrivacyAcked(bool v)            { m_directoryPrivacyAcked = v; }
    const std::string &getDirectoryStatusText() const { return m_directoryStatusText; }
    void setDirectoryStatusText(const std::string &v) { m_directoryStatusText = v; }
    // #84 — Currently-published directory streamId (empty when not
    // Active). Application mirrors DirectoryClient::getStreamId() here
    // every frame so APIController can embed it in /meta for the chat
    // panel on remote clients.
    const std::string &getDirectoryStreamId() const   { return m_directoryStreamId; }
    void setDirectoryStreamId(const std::string &v)   { m_directoryStreamId = v; }
    const std::string &getRemoteAuthToken() const  { return m_remoteAuthToken; }
    void setRemoteAuthToken(const std::string &v)  { m_remoteAuthToken = v; }

    // Directory telemetry getters/setters (#49 Phase 5).
    uint64_t getDirectoryRegisterOk()    const { return m_directoryRegisterOk; }
    uint64_t getDirectoryRegisterFail()  const { return m_directoryRegisterFail; }
    uint64_t getDirectoryHeartbeatOk()   const { return m_directoryHeartbeatOk; }
    uint64_t getDirectoryHeartbeatFail() const { return m_directoryHeartbeatFail; }
    uint64_t getDirectoryPatchOk()       const { return m_directoryPatchOk; }
    uint64_t getDirectoryPatchFail()     const { return m_directoryPatchFail; }
    int64_t  getDirectorySecondsSinceLastHeartbeat() const { return m_directorySecondsSinceLastHeartbeat; }
    void setDirectoryStats(uint64_t regOk, uint64_t regFail,
                           uint64_t hbOk, uint64_t hbFail,
                           uint64_t patchOk, uint64_t patchFail,
                           int64_t secondsSinceLastHeartbeat)
    {
        m_directoryRegisterOk    = regOk;
        m_directoryRegisterFail  = regFail;
        m_directoryHeartbeatOk   = hbOk;
        m_directoryHeartbeatFail = hbFail;
        m_directoryPatchOk       = patchOk;
        m_directoryPatchFail     = patchFail;
        m_directorySecondsSinceLastHeartbeat = secondsSinceLastHeartbeat;
    }
    void setStreamingPort(uint16_t port);
    void setStreamingWidth(uint32_t width) { m_streamingWidth = width; }
    void setStreamingHeight(uint32_t height) { m_streamingHeight = height; }
    void setStreamingFps(uint32_t fps) { m_streamingFps = fps; }
    void setStreamingBitrate(uint32_t bitrate) { m_streamingBitrate = bitrate; }
    void setStreamingAudioBitrate(uint32_t bitrate) { m_streamingAudioBitrate = bitrate; }
    void setStreamingVideoCodec(const std::string &codec) { m_streamingVideoCodec = codec; }
    void setStreamingAudioCodec(const std::string &codec) { m_streamingAudioCodec = codec; }
    void setStreamingH264Preset(const std::string &preset) { m_streamingH264Preset = preset; }
    void setStreamingH265Preset(const std::string &preset) { m_streamingH265Preset = preset; }
    void setStreamingH265Profile(const std::string &profile) { m_streamingH265Profile = profile; }
    void setStreamingH265Level(const std::string &level) { m_streamingH265Level = level; }
    void setStreamingVP8Speed(int speed) { m_streamingVP8Speed = speed; }
    void setStreamingVP9Speed(int speed) { m_streamingVP9Speed = speed; }
    // Hardware encoder selection — uses int so we don't have to pull
    // MediaEncoder.h into the UI layer. Stored as int matching the
    // MediaEncoder::HardwareEncoder enum values (Auto=0, Software=1,
    // NVENC=2, VAAPI=3, QSV=4, AMF=5).
    void setStreamingHardwareEncoder(int v) { m_streamingHardwareEncoder = v; }

    // Buffer settings
    void setStreamingMaxVideoBufferSize(size_t size) { m_streamingMaxVideoBufferSize = size; }
    void setStreamingMaxAudioBufferSize(size_t size) { m_streamingMaxAudioBufferSize = size; }
    void setStreamingMaxBufferTimeSeconds(int64_t seconds) { m_streamingMaxBufferTimeSeconds = seconds; }
    void setStreamingAVIOBufferSize(size_t size) { m_streamingAVIOBufferSize = size; }

    size_t getStreamingMaxVideoBufferSize() const { return m_streamingMaxVideoBufferSize; }
    size_t getStreamingMaxAudioBufferSize() const { return m_streamingMaxAudioBufferSize; }
    int64_t getStreamingMaxBufferTimeSeconds() const { return m_streamingMaxBufferTimeSeconds; }
    size_t getStreamingAVIOBufferSize() const { return m_streamingAVIOBufferSize; }

    // Streaming info getters (public)
    uint16_t getStreamingPort() const { return m_streamingPort; }
    uint32_t getStreamingWidth() const { return m_streamingWidth; }
    uint32_t getStreamingHeight() const { return m_streamingHeight; }
    uint32_t getStreamingFps() const { return m_streamingFps; }
    uint32_t getStreamingBitrate() const { return m_streamingBitrate; }
    uint32_t getStreamingAudioBitrate() const { return m_streamingAudioBitrate; }
    std::string getStreamingVideoCodec() const { return m_streamingVideoCodec; }
    std::string getStreamingAudioCodec() const { return m_streamingAudioCodec; }
    std::string getStreamingH264Preset() const { return m_streamingH264Preset; }
    std::string getStreamingH265Preset() const { return m_streamingH265Preset; }
    std::string getStreamingH265Profile() const { return m_streamingH265Profile; }
    std::string getStreamingH265Level() const { return m_streamingH265Level; }
    int getStreamingVP8Speed() const { return m_streamingVP8Speed; }
    int getStreamingVP9Speed() const { return m_streamingVP9Speed; }
    int getStreamingHardwareEncoder() const { return m_streamingHardwareEncoder; }
    std::string getStreamingNvencPreset() const { return m_streamingNvencPreset; }
    std::string getStreamingVaapiRcMode() const { return m_streamingVaapiRcMode; }
    std::string getStreamingQsvPreset()   const { return m_streamingQsvPreset; }
    std::string getStreamingAmfQuality()  const { return m_streamingAmfQuality; }
    std::string getRemoteInterpolation() const { return m_remoteInterpolation; }

    // Streaming setters com callbacks (para uso pelas classes de abas)
    void triggerStreamingPortChange(uint16_t port);
    void triggerStreamingWidthChange(uint32_t width);
    void triggerStreamingHeightChange(uint32_t height);
    void triggerStreamingFpsChange(uint32_t fps);
    void triggerStreamingBitrateChange(uint32_t bitrate);
    void triggerStreamingAudioBitrateChange(uint32_t bitrate);
    void triggerStreamingVideoCodecChange(const std::string &codec);
    void triggerStreamingAudioCodecChange(const std::string &codec);
    void triggerStreamingH264PresetChange(const std::string &preset);
    void triggerStreamingH265PresetChange(const std::string &preset);
    void triggerStreamingH265ProfileChange(const std::string &profile);
    void triggerStreamingH265LevelChange(const std::string &level);
    void triggerStreamingVP8SpeedChange(int speed);
    void triggerStreamingVP9SpeedChange(int speed);
    void triggerStreamingHardwareEncoderChange(int v);
    void triggerStreamingNvencPresetChange(const std::string &v);
    void triggerStreamingVaapiRcModeChange(const std::string &v);
    void triggerStreamingQsvPresetChange(const std::string &v);
    void triggerStreamingAmfQualityChange(const std::string &v);
    void triggerRemoteInterpolationChange(const std::string &v);
    void triggerStreamingMaxVideoBufferSizeChange(size_t size);
    void triggerStreamingMaxAudioBufferSizeChange(size_t size);
    void triggerStreamingMaxBufferTimeSecondsChange(int64_t seconds);
    void triggerStreamingAVIOBufferSizeChange(size_t size);
    void triggerStreamingStartStop(bool start);

    // Image settings getters
    float getBrightness() const { return m_brightness; }
    float getContrast() const { return m_contrast; }
    bool getMaintainAspect() const { return m_maintainAspect; }
    bool getFullscreen() const { return m_fullscreen; }
    int getMonitorIndex() const { return m_monitorIndex; }
    std::string getCurrentShader() const { return m_currentShader; }
    const std::vector<std::string> &getShaderList() const { return m_scannedShaders; }

    // Master shader bypass — when false the shader engine is skipped on
    // the live render path, so the user can A/B the effect without
    // losing the selected shader. The per-pipeline overrides below only
    // apply when the master is on; if the master is off all pipelines
    // see the raw source.
    bool getShaderPipelineEnabled() const { return m_shaderPipelineEnabled; }
    void setShaderPipelineEnabled(bool enabled) { m_shaderPipelineEnabled = enabled; }

    // Per-pipeline shader override. Only consulted when the master
    // pipeline toggle is on. False means "this pipeline pushes the raw
    // (pre-shader) source frame even though the live preview shows the
    // shader". Lets the user record clean video while streaming with
    // the CRT effect, or vice versa.
    bool getStreamingApplyShader() const { return m_streamingApplyShader; }
    void setStreamingApplyShader(bool apply) { m_streamingApplyShader = apply; }
    bool getRecordingApplyShader() const { return m_recordingApplyShader; }
    void setRecordingApplyShader(bool apply) { m_recordingApplyShader = apply; }

    // Capture info getters
    uint32_t getCaptureWidth() const { return m_captureWidth; }
    uint32_t getCaptureHeight() const { return m_captureHeight; }
    uint32_t getActualCaptureWidth() const { return m_actualCaptureWidth; }
    uint32_t getActualCaptureHeight() const { return m_actualCaptureHeight; }

    // Mirror of VideoCaptureRemote::isHostLikelyOffline() pushed by
    // Application every frame. Lives here because in Remote mode the
    // UIManager's m_capture pointer is null (Application passes
    // nullptr to setCaptureControls to suppress the V4L2/DS hardware
    // controls) so the Info panel can't dynamic-cast its way to the
    // flag. Default false in host mode (#58).
    bool getRemoteHostLikelyOffline() const { return m_remoteHostLikelyOffline; }
    void setRemoteHostLikelyOffline(bool v) { m_remoteHostLikelyOffline = v; }
    // 'Are we decoding frames right now' — distinct from
    // captureWidth > 0, which stays at the last seen value after
    // the stream drops. Mirrored by Application from
    // VideoCaptureRemote::isReceivingFrames() every frame.
    bool getRemoteReceivingFrames() const { return m_remoteReceivingFrames; }
    void setRemoteReceivingFrames(bool v) { m_remoteReceivingFrames = v; }

    // Host's current viewer count, parsed from /meta when we're in
    // client mode (#68). Application piggybacks the
    // RemoteMetaSync::Snapshot callback to keep this fresh. 0 when
    // we're not a client or the host's /meta predates this field.
    uint32_t getRemoteUpstreamClientCount() const { return m_remoteUpstreamClientCount; }
    void setRemoteUpstreamClientCount(uint32_t v) { m_remoteUpstreamClientCount = v; }
    float getSourceOverscanPercentX() const { return m_sourceOverscanPercentX; }
    float getSourceOverscanPercentY() const { return m_sourceOverscanPercentY; }
    bool getSourceOverscanLocked() const { return m_sourceOverscanLocked; }
    void setSourceOverscanPercentX(float pct) {
        m_sourceOverscanPercentX = pct;
        if (m_sourceOverscanLocked) m_sourceOverscanPercentY = pct;
    }
    void setSourceOverscanPercentY(float pct) {
        m_sourceOverscanPercentY = pct;
        if (m_sourceOverscanLocked) m_sourceOverscanPercentX = pct;
    }
    void setSourceOverscanLocked(bool locked) {
        m_sourceOverscanLocked = locked;
        if (locked) m_sourceOverscanPercentY = m_sourceOverscanPercentX;
    }
    uint32_t getCaptureFps() const { return m_captureFps; }
    std::string getCaptureDevice() const { return m_captureDevice; }
    IVideoCapture *getCapture() const { return m_capture; }
    
    // Audio capture
    void setAudioCapture(class IAudioCapture *audioCapture) { m_audioCapture = audioCapture; }
    class IAudioCapture *getAudioCapture() const { return m_audioCapture; }
    
    // Audio device configuration
    void setAudioInputSourceId(const std::string &sourceId) { m_audioInputSourceId = sourceId; }
    std::string getAudioInputSourceId() const { return m_audioInputSourceId; }

    // AVFoundation device + format persistence (macOS only). Stored
    // regardless of platform so a config saved on macOS round-trips
    // through Linux/Windows without losing data — they just ignore it.
    void setAVFoundationDeviceId(const std::string &id) { m_avfDeviceId = id; }
    std::string getAVFoundationDeviceId() const { return m_avfDeviceId; }
    void setAVFoundationFormatId(const std::string &id) { m_avfFormatId = id; }
    std::string getAVFoundationFormatId() const { return m_avfFormatId; }
    void setAVFoundationAudioDeviceId(const std::string &id) { m_avfAudioDeviceId = id; }
    std::string getAVFoundationAudioDeviceId() const { return m_avfAudioDeviceId; }

    // Streaming status getters
    bool getStreamingActive() const { return m_streamingActive; }
    std::string getStreamUrl() const { return m_streamUrl; }
    uint32_t getStreamClientCount() const { return m_streamClientCount; }
    bool canStartStreaming() const { return m_canStartStreaming; }
    int64_t getStreamingCooldownRemainingMs() const { return m_streamingCooldownRemainingMs; }

    // V4L2 getters
    const std::vector<std::string> &getV4L2Devices() const { return m_v4l2Devices; }
    const std::vector<V4L2Control> &getV4L2Controls() const { return m_v4l2Controls; }

    // DirectShow getters
    const std::vector<DeviceInfo> &getDSDevices() const { return m_dsDevices; }
    void refreshDSDevices();

    // Streaming callbacks
    void setOnStreamingStartStop(std::function<void(bool)> callback) { m_onStreamingStartStop = callback; }
    void setOnStreamingPortChanged(std::function<void(uint16_t)> callback) { m_onStreamingPortChanged = callback; }
    void setOnStreamingWidthChanged(std::function<void(uint32_t)> callback) { m_onStreamingWidthChanged = callback; }
    void setOnStreamingHeightChanged(std::function<void(uint32_t)> callback) { m_onStreamingHeightChanged = callback; }
    void setOnStreamingFpsChanged(std::function<void(uint32_t)> callback) { m_onStreamingFpsChanged = callback; }
    void setOnStreamingBitrateChanged(std::function<void(uint32_t)> callback) { m_onStreamingBitrateChanged = callback; }
    void setOnStreamingAudioBitrateChanged(std::function<void(uint32_t)> callback) { m_onStreamingAudioBitrateChanged = callback; }
    void setOnStreamingVideoCodecChanged(std::function<void(const std::string &)> callback) { m_onStreamingVideoCodecChanged = callback; }
    void setOnStreamingAudioCodecChanged(std::function<void(const std::string &)> callback) { m_onStreamingAudioCodecChanged = callback; }
    void setOnStreamingH264PresetChanged(std::function<void(const std::string &)> callback) { m_onStreamingH264PresetChanged = callback; }
    void setOnStreamingH265PresetChanged(std::function<void(const std::string &)> callback) { m_onStreamingH265PresetChanged = callback; }
    void setOnStreamingH265ProfileChanged(std::function<void(const std::string &)> callback) { m_onStreamingH265ProfileChanged = callback; }
    void setOnStreamingH265LevelChanged(std::function<void(const std::string &)> callback) { m_onStreamingH265LevelChanged = callback; }
    void setOnStreamingVP8SpeedChanged(std::function<void(int)> callback) { m_onStreamingVP8SpeedChanged = callback; }
    void setOnStreamingVP9SpeedChanged(std::function<void(int)> callback) { m_onStreamingVP9SpeedChanged = callback; }
    void setOnStreamingHardwareEncoderChanged(std::function<void(int)> callback) { m_onStreamingHardwareEncoderChanged = callback; }
    void setOnStreamingNvencPresetChanged(std::function<void(const std::string &)> cb) { m_onStreamingNvencPresetChanged = cb; }
    void setOnStreamingVaapiRcModeChanged(std::function<void(const std::string &)> cb) { m_onStreamingVaapiRcModeChanged = cb; }
    void setOnStreamingQsvPresetChanged  (std::function<void(const std::string &)> cb) { m_onStreamingQsvPresetChanged   = cb; }
    void setOnStreamingAmfQualityChanged (std::function<void(const std::string &)> cb) { m_onStreamingAmfQualityChanged  = cb; }
    void setOnRemoteInterpolationChanged (std::function<void(const std::string &)> cb) { m_onRemoteInterpolationChanged  = cb; }

    // Accessor used by Application to keep the connection-window's
    // capture pointer current — the window reads .isOpen() / dims to
    // decide whether to show Connect or Disconnect.
    UIRemoteConnection *getRemoteConnectionWindow() const { return m_remoteConnectionWindow.get(); }
    UIDirectoryBrowser *getDirectoryBrowserWindow() const { return m_directoryBrowserWindow.get(); }
    // OSD layer accessor — Application needs it to gate cursor
    // visibility on whether an interactive overlay is on screen (#68).
    QuickActionsOverlay *getQuickActionsOverlay() const { return m_quickActionsOverlay.get(); }
    class OSDChat       *getChatOverlay()        const { return m_chatOverlay.get(); }
    // Chat-overlay wiring (#84). Application creates the ChatClient
    // and hands the pointer here so the OSD has a transport to draw
    // from. Safe to call before or after init().
    void setChatClient(class ChatClient *chat);
    void setOnStreamingMaxVideoBufferSizeChanged(std::function<void(size_t)> callback) { m_onStreamingMaxVideoBufferSizeChanged = callback; }
    void setOnStreamingMaxAudioBufferSizeChanged(std::function<void(size_t)> callback) { m_onStreamingMaxAudioBufferSizeChanged = callback; }
    void setOnStreamingMaxBufferTimeSecondsChanged(std::function<void(int64_t)> callback) { m_onStreamingMaxBufferTimeSecondsChanged = callback; }
    void setOnStreamingAVIOBufferSizeChanged(std::function<void(size_t)> callback) { m_onStreamingAVIOBufferSizeChanged = callback; }

    // Recording info setters (public)
    void setRecordingActive(bool active) { m_recordingActive = active; }
    void setRecordingDurationUs(uint64_t durationUs) { m_recordingDurationUs = durationUs; }
    void setRecordingFileSize(uint64_t fileSize) { m_recordingFileSize = fileSize; }
    void setRecordingFilename(const std::string& filename) { m_recordingFilename = filename; }
    void setRecordingWidth(uint32_t width) { m_recordingWidth = width; }
    void setRecordingHeight(uint32_t height) { m_recordingHeight = height; }
    void setRecordingFps(uint32_t fps) { m_recordingFps = fps; }
    void setRecordingBitrate(uint32_t bitrate) { m_recordingBitrate = bitrate; }
    void setRecordingAudioBitrate(uint32_t bitrate) { m_recordingAudioBitrate = bitrate; }
    void setRecordingVideoCodec(const std::string& codec) { m_recordingVideoCodec = codec; }
    void setRecordingAudioCodec(const std::string& codec) { m_recordingAudioCodec = codec; }
    void setRecordingH264Preset(const std::string& preset) { m_recordingH264Preset = preset; }
    void setRecordingH265Preset(const std::string& preset) { m_recordingH265Preset = preset; }
    void setRecordingH265Profile(const std::string& profile) { m_recordingH265Profile = profile; }
    void setRecordingH265Level(const std::string& level) { m_recordingH265Level = level; }
    void setRecordingVP8Speed(int speed) { m_recordingVP8Speed = speed; }
    void setRecordingVP9Speed(int speed) { m_recordingVP9Speed = speed; }
    void setRecordingContainer(const std::string& container) { m_recordingContainer = container; }
    void setRecordingOutputPath(const std::string& path) { m_recordingOutputPath = path; }
    void setRecordingFilenameTemplate(const std::string& template_) { m_recordingFilenameTemplate = template_; }
    void setRecordingIncludeAudio(bool include) { m_recordingIncludeAudio = include; }

    // Hardware encoder selection for recording (#59). Same int-based
    // encoding as the streaming side so we don't have to pull
    // MediaEncoder.h into the UI layer (0=Auto, 1=Software,
    // 2=NVENC, 3=VAAPI, 4=QSV, 5=AMF). Backend-specific preset
    // strings live in separate fields per backend, mirroring the
    // streaming layout exactly so the same UI block can render both.
    void setRecordingHardwareEncoder(int v)           { m_recordingHardwareEncoder = v; }
    void setRecordingNvencPreset(const std::string &v){ m_recordingNvencPreset = v; }
    void setRecordingVaapiRcMode(const std::string &v){ m_recordingVaapiRcMode = v; }
    void setRecordingQsvPreset(const std::string &v)  { m_recordingQsvPreset = v; }
    void setRecordingAmfQuality(const std::string &v) { m_recordingAmfQuality = v; }

    // Recording info getters (public)
    bool getRecordingActive() const { return m_recordingActive; }
    uint64_t getRecordingDurationUs() const { return m_recordingDurationUs; }
    uint64_t getRecordingFileSize() const { return m_recordingFileSize; }
    std::string getRecordingFilename() const { return m_recordingFilename; }
    uint32_t getRecordingWidth() const { return m_recordingWidth; }
    uint32_t getRecordingHeight() const { return m_recordingHeight; }
    uint32_t getRecordingFps() const { return m_recordingFps; }
    uint32_t getRecordingBitrate() const { return m_recordingBitrate; }
    uint32_t getRecordingAudioBitrate() const { return m_recordingAudioBitrate; }
    std::string getRecordingVideoCodec() const { return m_recordingVideoCodec; }
    std::string getRecordingAudioCodec() const { return m_recordingAudioCodec; }
    std::string getRecordingH264Preset() const { return m_recordingH264Preset; }
    std::string getRecordingH265Preset() const { return m_recordingH265Preset; }
    std::string getRecordingH265Profile() const { return m_recordingH265Profile; }
    std::string getRecordingH265Level() const { return m_recordingH265Level; }
    int getRecordingVP8Speed() const { return m_recordingVP8Speed; }
    int getRecordingVP9Speed() const { return m_recordingVP9Speed; }
    std::string getRecordingContainer() const { return m_recordingContainer; }
    std::string getRecordingOutputPath() const { return m_recordingOutputPath; }
    std::string getRecordingFilenameTemplate() const { return m_recordingFilenameTemplate; }
    bool getRecordingIncludeAudio() const { return m_recordingIncludeAudio; }
    int  getRecordingHardwareEncoder() const         { return m_recordingHardwareEncoder; }
    std::string getRecordingNvencPreset() const      { return m_recordingNvencPreset; }
    std::string getRecordingVaapiRcMode() const      { return m_recordingVaapiRcMode; }
    std::string getRecordingQsvPreset()   const      { return m_recordingQsvPreset; }
    std::string getRecordingAmfQuality()  const      { return m_recordingAmfQuality; }

    // Recording setters with callbacks
    void triggerRecordingWidthChange(uint32_t width);
    void triggerRecordingHeightChange(uint32_t height);
    void triggerRecordingFpsChange(uint32_t fps);
    void triggerRecordingBitrateChange(uint32_t bitrate);
    void triggerRecordingAudioBitrateChange(uint32_t bitrate);
    void triggerRecordingVideoCodecChange(const std::string& codec);
    void triggerRecordingAudioCodecChange(const std::string& codec);
    void triggerRecordingH264PresetChange(const std::string& preset);
    void triggerRecordingH265PresetChange(const std::string& preset);
    void triggerRecordingH265ProfileChange(const std::string& profile);
    void triggerRecordingH265LevelChange(const std::string& level);
    void triggerRecordingVP8SpeedChange(int speed);
    void triggerRecordingVP9SpeedChange(int speed);
    void triggerRecordingContainerChange(const std::string& container);
    void triggerRecordingOutputPathChange(const std::string& path);
    void triggerRecordingFilenameTemplateChange(const std::string& template_);
    void triggerRecordingIncludeAudioChange(bool include);
    void triggerRecordingHardwareEncoderChange(int v);
    void triggerRecordingNvencPresetChange(const std::string &v);
    void triggerRecordingVaapiRcModeChange(const std::string &v);
    void triggerRecordingQsvPresetChange(const std::string &v);
    void triggerRecordingAmfQualityChange(const std::string &v);
    void triggerRecordingStartStop(bool start);

    // Recording profiles — save/load/list/delete the full recording
    // configuration (codec, preset, bitrate, container, audio, etc.)
    // as named profiles under $XDG_DATA_HOME/retrocapture/recording_profiles.
    std::vector<std::string> listRecordingProfiles();
    bool saveRecordingProfile(const std::string& name);
    bool loadRecordingProfile(const std::string& name);
    bool deleteRecordingProfile(const std::string& name);
    bool recordingProfileExists(const std::string& name);

    // Streaming profiles — analogous to recording profiles, persisting
    // the streaming configuration (codec, preset, bitrate, port, audio,
    // buffer tuning) under $XDG_DATA_HOME/retrocapture/streaming_profiles.
    std::vector<std::string> listStreamingProfiles();
    bool saveStreamingProfile(const std::string& name);
    bool loadStreamingProfile(const std::string& name);
    bool deleteStreamingProfile(const std::string& name);
    bool streamingProfileExists(const std::string& name);

    // Recording callbacks
    void setOnRecordingStartStop(std::function<void(bool)> callback) { m_onRecordingStartStop = callback; }
    void setOnRecordingWidthChanged(std::function<void(uint32_t)> callback) { m_onRecordingWidthChanged = callback; }
    void setOnRecordingHeightChanged(std::function<void(uint32_t)> callback) { m_onRecordingHeightChanged = callback; }
    void setOnRecordingFpsChanged(std::function<void(uint32_t)> callback) { m_onRecordingFpsChanged = callback; }
    void setOnRecordingBitrateChanged(std::function<void(uint32_t)> callback) { m_onRecordingBitrateChanged = callback; }
    void setOnRecordingAudioBitrateChanged(std::function<void(uint32_t)> callback) { m_onRecordingAudioBitrateChanged = callback; }
    void setOnRecordingVideoCodecChanged(std::function<void(const std::string&)> callback) { m_onRecordingVideoCodecChanged = callback; }
    void setOnRecordingAudioCodecChanged(std::function<void(const std::string&)> callback) { m_onRecordingAudioCodecChanged = callback; }
    void setOnRecordingH264PresetChanged(std::function<void(const std::string&)> callback) { m_onRecordingH264PresetChanged = callback; }
    void setOnRecordingH265PresetChanged(std::function<void(const std::string&)> callback) { m_onRecordingH265PresetChanged = callback; }
    void setOnRecordingH265ProfileChanged(std::function<void(const std::string&)> callback) { m_onRecordingH265ProfileChanged = callback; }
    void setOnRecordingH265LevelChanged(std::function<void(const std::string&)> callback) { m_onRecordingH265LevelChanged = callback; }
    void setOnRecordingVP8SpeedChanged(std::function<void(int)> callback) { m_onRecordingVP8SpeedChanged = callback; }
    void setOnRecordingVP9SpeedChanged(std::function<void(int)> callback) { m_onRecordingVP9SpeedChanged = callback; }
    void setOnRecordingContainerChanged(std::function<void(const std::string&)> callback) { m_onRecordingContainerChanged = callback; }
    void setOnRecordingOutputPathChanged(std::function<void(const std::string&)> callback) { m_onRecordingOutputPathChanged = callback; }
    void setOnRecordingFilenameTemplateChanged(std::function<void(const std::string&)> callback) { m_onRecordingFilenameTemplateChanged = callback; }
    void setOnRecordingIncludeAudioChanged(std::function<void(bool)> callback) { m_onRecordingIncludeAudioChanged = callback; }
    void setOnRecordingHardwareEncoderChanged(std::function<void(int)> cb)             { m_onRecordingHardwareEncoderChanged = cb; }
    void setOnRecordingNvencPresetChanged(std::function<void(const std::string &)> cb) { m_onRecordingNvencPresetChanged = cb; }
    void setOnRecordingVaapiRcModeChanged(std::function<void(const std::string &)> cb) { m_onRecordingVaapiRcModeChanged = cb; }
    void setOnRecordingQsvPresetChanged  (std::function<void(const std::string &)> cb) { m_onRecordingQsvPresetChanged = cb; }
    void setOnRecordingAmfQualityChanged (std::function<void(const std::string &)> cb) { m_onRecordingAmfQualityChanged = cb; }

    // Web Portal settings
    void setWebPortalEnabled(bool enabled) { m_webPortalEnabled = enabled; }
    void setWebPortalHTTPSEnabled(bool enabled) { m_webPortalHTTPSEnabled = enabled; }
    void setWebPortalSSLCertPath(const std::string &path) { m_webPortalSSLCertPath = path; }
    void setWebPortalSSLKeyPath(const std::string &path) { m_webPortalSSLKeyPath = path; }
    void setFoundSSLCertificatePath(const std::string &path) { m_foundSSLCertPath = path; }
    void setFoundSSLKeyPath(const std::string &path) { m_foundSSLKeyPath = path; }
    void setWebPortalTitle(const std::string &title) { m_webPortalTitle = title; }
    void setWebPortalImagePath(const std::string &path) { m_webPortalImagePath = path; }
    void setWebPortalBackgroundImagePath(const std::string &path) { m_webPortalBackgroundImagePath = path; }
    void setWebPortalColorBackground(const float color[4])
    {
        if (color)
            memcpy(m_webPortalColorBackground, color, 4 * sizeof(float));
    }
    void setWebPortalColorText(const float color[4])
    {
        if (color)
            memcpy(m_webPortalColorText, color, 4 * sizeof(float));
    }
    void setWebPortalColorPrimary(const float color[4])
    {
        if (color)
            memcpy(m_webPortalColorPrimary, color, 4 * sizeof(float));
    }
    void setWebPortalColorSecondary(const float color[4])
    {
        if (color)
            memcpy(m_webPortalColorSecondary, color, 4 * sizeof(float));
    }
    void setWebPortalColorCardHeader(const float color[4])
    {
        if (color)
            memcpy(m_webPortalColorCardHeader, color, 4 * sizeof(float));
    }
    void setWebPortalColorBorder(const float color[4])
    {
        if (color)
            memcpy(m_webPortalColorBorder, color, 4 * sizeof(float));
    }
    void setWebPortalColorSuccess(const float color[4])
    {
        if (color)
            memcpy(m_webPortalColorSuccess, color, 4 * sizeof(float));
    }
    void setWebPortalColorWarning(const float color[4])
    {
        if (color)
            memcpy(m_webPortalColorWarning, color, 4 * sizeof(float));
    }
    void setWebPortalColorDanger(const float color[4])
    {
        if (color)
            memcpy(m_webPortalColorDanger, color, 4 * sizeof(float));
    }

    bool getWebPortalEnabled() const { return m_webPortalEnabled; }
    bool getWebPortalHTTPSEnabled() const { return m_webPortalHTTPSEnabled; }
    std::string getWebPortalSSLCertPath() const { return m_webPortalSSLCertPath; }
    std::string getWebPortalSSLKeyPath() const { return m_webPortalSSLKeyPath; }
    std::string getFoundSSLCertificatePath() const { return m_foundSSLCertPath; }
    std::string getFoundSSLKeyPath() const { return m_foundSSLKeyPath; }
    std::string getWebPortalTitle() const { return m_webPortalTitle; }
    std::string getWebPortalSubtitle() const { return m_webPortalSubtitle; }
    std::string getWebPortalImagePath() const { return m_webPortalImagePath; }
    std::string getWebPortalBackgroundImagePath() const { return m_webPortalBackgroundImagePath; }

    // Getters para textos editáveis
    std::string getWebPortalTextStreamInfo() const { return m_webPortalTextStreamInfo; }
    std::string getWebPortalTextQuickActions() const { return m_webPortalTextQuickActions; }
    std::string getWebPortalTextCompatibility() const { return m_webPortalTextCompatibility; }
    std::string getWebPortalTextStatus() const { return m_webPortalTextStatus; }
    std::string getWebPortalTextCodec() const { return m_webPortalTextCodec; }
    std::string getWebPortalTextResolution() const { return m_webPortalTextResolution; }
    std::string getWebPortalTextStreamUrl() const { return m_webPortalTextStreamUrl; }
    std::string getWebPortalTextCopyUrl() const { return m_webPortalTextCopyUrl; }
    std::string getWebPortalTextOpenNewTab() const { return m_webPortalTextOpenNewTab; }
    std::string getWebPortalTextSupported() const { return m_webPortalTextSupported; }
    std::string getWebPortalTextFormat() const { return m_webPortalTextFormat; }
    std::string getWebPortalTextCodecInfo() const { return m_webPortalTextCodecInfo; }
    std::string getWebPortalTextSupportedBrowsers() const { return m_webPortalTextSupportedBrowsers; }
    std::string getWebPortalTextFormatInfo() const { return m_webPortalTextFormatInfo; }
    std::string getWebPortalTextCodecInfoValue() const { return m_webPortalTextCodecInfoValue; }
    std::string getWebPortalTextConnecting() const { return m_webPortalTextConnecting; }

    // Getters para cores
    const float *getWebPortalColorBackground() const { return m_webPortalColorBackground; }
    const float *getWebPortalColorText() const { return m_webPortalColorText; }
    const float *getWebPortalColorPrimary() const { return m_webPortalColorPrimary; }
    const float *getWebPortalColorPrimaryLight() const { return m_webPortalColorPrimaryLight; }
    const float *getWebPortalColorPrimaryDark() const { return m_webPortalColorPrimaryDark; }
    const float *getWebPortalColorSecondary() const { return m_webPortalColorSecondary; }
    const float *getWebPortalColorSecondaryHighlight() const { return m_webPortalColorSecondaryHighlight; }
    const float *getWebPortalColorCardHeader() const { return m_webPortalColorCardHeader; }
    const float *getWebPortalColorBorder() const { return m_webPortalColorBorder; }
    const float *getWebPortalColorSuccess() const { return m_webPortalColorSuccess; }
    const float *getWebPortalColorWarning() const { return m_webPortalColorWarning; }
    const float *getWebPortalColorDanger() const { return m_webPortalColorDanger; }
    const float *getWebPortalColorInfo() const { return m_webPortalColorInfo; }
    void setOnWebPortalEnabledChanged(std::function<void(bool)> callback) { m_onWebPortalEnabledChanged = callback; }
    void setOnWebPortalHTTPSChanged(std::function<void(bool)> callback) { m_onWebPortalHTTPSChanged = callback; }
    void setOnWebPortalSSLCertPathChanged(std::function<void(const std::string &)> callback) { m_onWebPortalSSLCertPathChanged = callback; }
    void setOnWebPortalSSLKeyPathChanged(std::function<void(const std::string &)> callback) { m_onWebPortalSSLKeyPathChanged = callback; }
    void setOnWebPortalTitleChanged(std::function<void(const std::string &)> callback) { m_onWebPortalTitleChanged = callback; }
    void setOnWebPortalSubtitleChanged(std::function<void(const std::string &)> callback) { m_onWebPortalSubtitleChanged = callback; }
    void setOnWebPortalImagePathChanged(std::function<void(const std::string &)> callback) { m_onWebPortalImagePathChanged = callback; }
    void setOnWebPortalBackgroundImagePathChanged(std::function<void(const std::string &)> callback) { m_onWebPortalBackgroundImagePathChanged = callback; }
    void setOnWebPortalColorsChanged(std::function<void()> callback) { m_onWebPortalColorsChanged = callback; }
    void setOnWebPortalTextsChanged(std::function<void()> callback) { m_onWebPortalTextsChanged = callback; }

    // Web Portal active state (independent from streaming)
    void setWebPortalActive(bool active) { m_webPortalActive = active; }
    bool getWebPortalActive() const { return m_webPortalActive; }
    void setOnWebPortalStartStop(std::function<void(bool)> callback) { m_onWebPortalStartStop = callback; }

    // Web Portal setters com callbacks (para uso pelas classes de abas)
    void triggerWebPortalEnabledChange(bool enabled);
    void triggerWebPortalHTTPSChange(bool enabled);
    void triggerWebPortalStartStop(bool start);
    void triggerWebPortalTitleChange(const std::string &title);
    void triggerWebPortalSubtitleChange(const std::string &subtitle);
    void triggerWebPortalSSLCertPathChange(const std::string &path);
    void triggerWebPortalSSLKeyPathChange(const std::string &path);
    void triggerWebPortalBackgroundImagePathChange(const std::string &path);
    void triggerWebPortalColorsChange();
    void triggerWebPortalTextsChange();

    // Getters para cores editáveis (retornam ponteiros não-const)
    float *getWebPortalColorBackgroundEditable() { return m_webPortalColorBackground; }
    float *getWebPortalColorTextEditable() { return m_webPortalColorText; }
    float *getWebPortalColorPrimaryEditable() { return m_webPortalColorPrimary; }
    float *getWebPortalColorPrimaryLightEditable() { return m_webPortalColorPrimaryLight; }
    float *getWebPortalColorPrimaryDarkEditable() { return m_webPortalColorPrimaryDark; }
    float *getWebPortalColorSecondaryEditable() { return m_webPortalColorSecondary; }
    float *getWebPortalColorSecondaryHighlightEditable() { return m_webPortalColorSecondaryHighlight; }
    float *getWebPortalColorCardHeaderEditable() { return m_webPortalColorCardHeader; }
    float *getWebPortalColorBorderEditable() { return m_webPortalColorBorder; }
    float *getWebPortalColorSuccessEditable() { return m_webPortalColorSuccess; }
    float *getWebPortalColorWarningEditable() { return m_webPortalColorWarning; }
    float *getWebPortalColorDangerEditable() { return m_webPortalColorDanger; }
    float *getWebPortalColorInfoEditable() { return m_webPortalColorInfo; }

    // Getters para textos editáveis (retornam referências não-const)
    std::string &getWebPortalTextStreamInfoEditable() { return m_webPortalTextStreamInfo; }
    std::string &getWebPortalTextQuickActionsEditable() { return m_webPortalTextQuickActions; }
    std::string &getWebPortalTextCompatibilityEditable() { return m_webPortalTextCompatibility; }
    std::string &getWebPortalTextStatusEditable() { return m_webPortalTextStatus; }
    std::string &getWebPortalTextCodecEditable() { return m_webPortalTextCodec; }
    std::string &getWebPortalTextResolutionEditable() { return m_webPortalTextResolution; }
    std::string &getWebPortalTextStreamUrlEditable() { return m_webPortalTextStreamUrl; }
    std::string &getWebPortalTextCopyUrlEditable() { return m_webPortalTextCopyUrl; }
    std::string &getWebPortalTextOpenNewTabEditable() { return m_webPortalTextOpenNewTab; }
    std::string &getWebPortalTextSupportedEditable() { return m_webPortalTextSupported; }
    std::string &getWebPortalTextFormatEditable() { return m_webPortalTextFormat; }
    std::string &getWebPortalTextCodecInfoEditable() { return m_webPortalTextCodecInfo; }
    std::string &getWebPortalTextSupportedBrowsersEditable() { return m_webPortalTextSupportedBrowsers; }
    std::string &getWebPortalTextFormatInfoEditable() { return m_webPortalTextFormatInfo; }
    std::string &getWebPortalTextCodecInfoValueEditable() { return m_webPortalTextCodecInfoValue; }
    std::string &getWebPortalTextConnectingEditable() { return m_webPortalTextConnecting; }

private:
    bool m_initialized = false;
    bool m_uiVisible = true;

    // Standalone configuration windows. Previously hosted as tabs
    // inside the unified "RetroCapture Controls" window; split out
    // for clarity (Fase A of #45 + window restructure). Each one
    // owns its own m_visible and ImGui::Begin/End.
    std::unique_ptr<class UIConfigurationSource>    m_sourceWindow;
    std::unique_ptr<class UIConfigurationShader>    m_shaderWindow;
    std::unique_ptr<class UIConfigurationImage>     m_imageWindow;
    std::unique_ptr<class UIConfigurationStreaming> m_streamingWindow;
#if defined(__linux__)
    std::unique_ptr<class UIConfigurationVirtualCamera> m_virtcamWindow;
#endif
    std::unique_ptr<class UIConfigurationRecording> m_recordingWindow;
    std::unique_ptr<class UIConfigurationWebPortal> m_webPortalWindow;
#if defined(__linux__) || defined(__APPLE__)
    std::unique_ptr<class UIConfigurationAudio>     m_audioWindow;
#endif
    std::unique_ptr<class UIInfoPanel>              m_infoWindow;
    std::unique_ptr<class UIPreferences>            m_preferencesWindow;
    std::unique_ptr<class UIShortcutsHelp>          m_shortcutsHelpWindow;
    // OSD layer (#68) — lives in src/osd/, owned by UIManager since
    // it has the only natural place to hand them lifetime + state
    // accessors.
    std::unique_ptr<class QuickActionsOverlay>      m_quickActionsOverlay;
    std::unique_ptr<class ConnectionStatusOverlay>  m_connectionOverlay;
    // Chat overlay (#84) — OSD layer like the two above, owns its
    // input/scroll state but reads message data from the shared
    // ChatClient (held by Application; pointer wired via
    // setChatClient).
    std::unique_ptr<class OSDChat>                  m_chatOverlay;
    class ChatClient                               *m_chatClient = nullptr;
    // Loaded from config.json before m_quickActionsOverlay exists,
    // applied via setVisible() right after construction. Defaults to
    // true so first-time users see the overlay; subsequent toggles
    // round-trip through saveConfig().
    bool m_quickActionsVisible = true;
    bool m_chatOverlayVisible  = true;
    // Same persistence pattern for the shortcuts-help orientation
    // widget (#68 follow-up). Default true so new users see the
    // keyboard hints on first launch.
    bool m_shortcutsHelpVisible = true;

    std::unique_ptr<class UICredits> m_creditsWindow;
    std::unique_ptr<class UICapturePresets> m_capturePresetsWindow;
    std::unique_ptr<class UIRecordings> m_recordingsWindow;
    std::unique_ptr<class UIRemoteConnection> m_remoteConnectionWindow;
    std::unique_ptr<class UIDirectoryBrowser> m_directoryBrowserWindow;

    // ImGui's IO holds a raw pointer to the ini path string; keep the
    // backing storage alive on UIManager for the whole lifetime.
    std::string m_iniPath;
    std::unique_ptr<RecordingProfileManager> m_recordingProfileManager;
    std::unique_ptr<StreamingProfileManager> m_streamingProfileManager;
    void *m_window = nullptr; // GLFWwindow* or SDL_Window*

    // Master shader pipeline toggle. When false, applyShader is
    // skipped on the live render path so the user can compare the
    // effect on/off without dropping the selected shader.
    bool m_shaderPipelineEnabled = true;
    // Per-pipeline shader application. False = pipeline pushes the raw
    // pre-shader source even though the master is on.
    bool m_streamingApplyShader = true;
    bool m_recordingApplyShader = true;

    // Shader selection
    std::vector<std::string> m_shaderList;
    std::string m_currentShader;
    int m_selectedShaderIndex = 0;
    std::function<void(const std::string &)> m_onShaderChanged;
    std::function<void(bool)> m_onVisibilityChanged;
    ShaderEngine *m_shaderEngine = nullptr;

    // Brightness/Contrast
    float m_brightness = 1.0f;
    float m_contrast = 1.0f;
    std::function<void(float)> m_onBrightnessChanged;
    std::function<void(float)> m_onContrastChanged;

    // Aspect ratio
    bool m_maintainAspect = false;
    std::function<void(bool)> m_onMaintainAspectChanged;

    // Fullscreen
    bool m_fullscreen = false;
    int m_monitorIndex = -1; // -1 = usar monitor primário
    std::function<void(bool)> m_onFullscreenChanged;
    std::function<void(int)> m_onMonitorIndexChanged;
    
    // Resolução de saída configurável (0 = automático, usar source)
    uint32_t m_outputWidth = 0;
    uint32_t m_outputHeight = 0;
    std::function<void(uint32_t, uint32_t)> m_onOutputResolutionChanged;

    // V4L2 Controls
    IVideoCapture *m_capture = nullptr;
    IAudioCapture *m_audioCapture = nullptr;
    
    // Audio device configuration (saved/loaded from config)
    std::string m_audioInputSourceId;
    // AVFoundation persistence (macOS). Stored on all platforms so
    // configs round-trip cleanly between machines.
    std::string m_avfDeviceId;
    std::string m_avfFormatId;
    std::string m_avfAudioDeviceId;
    std::vector<V4L2Control> m_v4l2Controls;
    std::function<void(const std::string &, int32_t)> m_onV4L2ControlChanged;

    // Source selection
#ifdef _WIN32
    SourceType m_sourceType = SourceType::DS; // Padrão: DirectShow no Windows
#elif defined(__APPLE__)
    SourceType m_sourceType = SourceType::AVFoundation; // Padrão: AVFoundation no macOS
#else
    SourceType m_sourceType = SourceType::V4L2; // Padrão: V4L2 no Linux
#endif

    // Device selection (V4L2)
    std::vector<std::string> m_v4l2Devices;
    std::vector<DeviceInfo> m_dsDevices;
    std::string m_currentDevice;
    std::function<void(const std::string &)> m_onDeviceChanged;
    std::function<void(SourceType)> m_onSourceTypeChanged;

    // Capture info: m_captureWidth/Height são a resolução LÓGICA (o que o user
    // pediu via UI). m_actualCaptureWidth/Height refletem o que o V4L2 entregou
    // depois do ajuste. Quando diferem, o pipeline faz downscale antes do shader.
    uint32_t m_captureWidth = 0;
    uint32_t m_captureHeight = 0;
    uint32_t m_actualCaptureWidth = 0;
    uint32_t m_actualCaptureHeight = 0;
    uint32_t m_captureFps = 0;
    bool     m_remoteHostLikelyOffline = false;
    bool     m_remoteReceivingFrames   = false;
    uint32_t m_remoteUpstreamClientCount = 0;
    // Connection-overlay frame-to-frame tracking. We detect
    // Connection-overlay transition tracking moved to
    // osd::ConnectionStatusOverlay during the OSD layer pass (#68).
    // Overscan: crop % das bordas do source antes do downscale.
    // X horizontal, Y vertical. Locked espelha um no outro.
    float m_sourceOverscanPercentX = 0.0f;
    float m_sourceOverscanPercentY = 0.0f;
    bool m_sourceOverscanLocked = true;
    std::string m_captureDevice;
    std::function<void(uint32_t, uint32_t)> m_onResolutionChanged;
    std::function<void(uint32_t)> m_onFramerateChanged;

    // UI helpers (tornados públicos para uso pelas classes de abas)
public:
    void renderShaderPanel();
    void renderImageControls(); // Mantido temporariamente para compatibilidade
    void renderSourcePanel();
    void renderV4L2Controls();
    void renderInfoPanel();
    void renderStreamingPanel();
    void renderWebPortalPanel();

    // Configuration persistence (tornados públicos para uso pelas classes de abas)
    void saveConfig();
    void loadConfig();
    std::string getConfigPath() const;

    // Scanning methods (tornados públicos para uso pelas classes de abas)
    void scanShaders(const std::string &basePath);

private:
    void scanV4L2Devices();

    std::vector<std::string> m_scannedShaders;
    std::string m_shaderBasePath = "shaders/shaders_glsl";

    // Save preset
    std::function<void(const std::string &, bool)> m_onSavePreset; // path, overwrite
    char m_savePresetPath[512] = "";
    bool m_showSaveDialog = false;

    // Streaming controls
    bool m_streamingEnabled = false;
    uint16_t m_streamingPort = 8080;
    uint32_t m_streamingWidth = 640;  // Padrão: 640px
    uint32_t m_streamingHeight = 480; // Padrão: 480px
    uint32_t m_streamingFps = 60;
    uint32_t m_streamingBitrate = 8000;
    uint32_t m_streamingAudioBitrate = 256;
    std::string m_streamingVideoCodec = "h264";
    std::string m_streamingAudioCodec = "aac";
    std::string m_streamingH264Preset = "veryfast"; // Preset H.264: ultrafast, superfast, veryfast, faster, fast, medium, slow, slower, veryslow
    std::string m_streamingH265Preset = "veryfast"; // Preset H.265: ultrafast, superfast, veryfast, faster, fast, medium, slow, slower, veryslow
    std::string m_streamingH265Profile = "main";    // Profile H.265: "main" (8-bit) ou "main10" (10-bit)
    std::string m_streamingH265Level = "auto";      // Level H.265: "auto", "1", "2", "2.1", "3", "3.1", "4", "4.1", "5", "5.1", "5.2", "6", "6.1", "6.2"
    int m_streamingVP8Speed = 12;                   // Speed VP8: 0-16 (0 = melhor qualidade, 16 = mais rápido, 12 = bom para streaming)
    int m_streamingVP9Speed = 6;                    // Speed VP9: 0-9 (0 = melhor qualidade, 9 = mais rápido, 6 = bom para streaming)
    int m_streamingHardwareEncoder = 0;             // 0 = Auto (matches MediaEncoder::HardwareEncoder::Auto)
    // Per-backend quality / preset values — the Streaming-tab UI shows
    // whichever combo matches the currently selected hardware encoder.
    // Stored separately so switching encoders preserves each backend's
    // previously chosen value rather than collapsing them onto one
    // shared string whose meaning would shift mid-flight.
    std::string m_streamingNvencPreset = "p4";      // p1 (fastest) .. p7 (slowest)
    std::string m_streamingVaapiRcMode = "CBR";     // CBR / VBR / CQP
    std::string m_streamingQsvPreset   = "veryfast";// libx264-style names
    std::string m_streamingAmfQuality  = "speed";   // speed / balanced / quality

    // Client-side interpolation mode for Remote-source playback. Picks
    // how each display refresh resolves the time between two consecutive
    // stream frames (see VideoCaptureRemote::captureLatestFrame):
    //   "linear"  — LERP between prev and next (smooth motion, ghosting)
    //   "nearest" — show the closer frame (clean image, 3:2 stutter)
    //   "off"     — strict PTS gate, hold prev until next is due
    std::string m_remoteInterpolation = "linear";
    bool m_streamingActive = false;
    std::string m_streamUrl = "";
    uint32_t m_streamClientCount = 0;
    bool m_canStartStreaming = true;            // Pode iniciar streaming (não está em cooldown)
    int64_t m_streamingCooldownRemainingMs = 0; // Tempo restante de cooldown em ms
    bool m_streamingProcessing = false;         // Flag para indicar que start/stop está sendo processado

    // Public-directory publish settings (#49 Phase 2). State here is
    // UI-side only; Application owns the DirectoryClient that
    // actually talks to the directory service and mirrors the toggle
    // from here every frame.
    bool        m_directoryPublishEnabled = false;
    std::string m_directoryUrl            = "https://directory.retrocapture.com";
    // #84 — Chat URL. Production default; --chat-url overrides at
    // launch; the Streaming → Advanced field overrides at runtime.
    // Accepts https://, http://, wss://, ws:// — ChatClient
    // normalizes the scheme when building REST vs WS endpoints.
    std::string m_chatBaseUrl             = "https://chat.retrocapture.com";
    // #84 — Persistent chat display name. Default empty; OSD chat
    // panel's Apply button writes here + saveConfig.
    std::string m_chatNickname            = "";
    // #84 — One-shot flag: UIConfigurationStreaming raises it when
    // the user clicks "Configure Profile" from the streaming
    // settings; OSDChat consumes it on the next frame and opens
    // the Profile window even if the chat panel itself is hidden.
    bool        m_openChatProfileRequested = false;
    bool        m_openChatRoomsRequested   = false;
    // #84 — Open the chat alongside the stream. ON by default for
    // backwards-compatible behaviour.
    bool        m_streamChatEnabled       = true;
    // #84 — Human-readable title for the streamer's chat room.
    std::string m_streamRoomTitle         = "";
    // #84 — The streamer's persistent room slug, auto-derived at
    // provision time. Provisioned on the first stream-with-chat
    // start, then reused forever.
    std::string m_streamRoomSlug          = "";
    // #85 — Virtual camera config + runtime status.
    bool        m_virtcamEnabled          = false;
    std::string m_virtcamDevicePath       = "";       // empty = auto-pick first loopback
    uint32_t    m_virtcamOutputWidth      = 0;        // 0 = follow shader output
    uint32_t    m_virtcamOutputHeight     = 0;
    uint32_t    m_virtcamOutputFps        = 0;        // 0 = follow capture FPS
    std::string m_virtcamPixelFormat      = "yuyv";   // "yuyv" | "rgb24"
    std::string m_virtcamStatusText;                  // populated by Application
    std::string m_virtcamErrorText;
    bool        m_virtcamStopRequested    = false;    // UI -> Application
    bool        m_virtcamStoppedNotice    = false;    // Application -> UI
    // Dev-only: skip TLS peer-certificate verification when talking to
    // the directory. Off by default; toggled from Streaming → Public
    // Directory → Advanced. Never persisted as ON for the public host.
    bool        m_directoryInsecureSkipVerify = false;
    // Preferences (#45 placeholder + window restructure)
    std::string m_language                = "en";    // "en" | "pt"
    bool        m_startFullscreen         = false;
    std::string m_directoryStreamName     = "";
    std::string m_directoryHostNickname   = "";
    std::string m_directoryPassword       = "";       // optional; empty = no password
    std::string m_directoryEndpointMode   = "direct"; // "direct" | "tunnel-cloudflare" | "custom"
    std::string m_directoryCustomEndpoint = "";       // used when mode == "custom"
    // Phase 2.5c (#60): Cloudflare Tunnel sub-mode + Named-tunnel state.
    std::string m_directoryTunnelMode           = "quick"; // "quick" | "named"
    std::string m_directoryNamedTunnelId        = "";       // cloudflared tunnel uuid
    std::string m_directoryNamedTunnelHostname  = "";       // user's own hostname (e.g. stream.example.com)
    bool        m_directoryPrivacyAcked   = false;    // sticky once the user accepts the warning
    std::string m_directoryStatusText     = "Idle";   // surfaced by Application; UI just reads
    std::string m_directoryStreamId       = "";       // #84 — mirrored from DirectoryClient for /meta

    // Per-session telemetry counters mirrored from DirectoryClient
    // (#49 Phase 5). Application writes each frame; UI reads.
    uint64_t m_directoryRegisterOk     = 0;
    uint64_t m_directoryRegisterFail   = 0;
    uint64_t m_directoryHeartbeatOk    = 0;
    uint64_t m_directoryHeartbeatFail  = 0;
    uint64_t m_directoryPatchOk        = 0;
    uint64_t m_directoryPatchFail      = 0;
    int64_t  m_directorySecondsSinceLastHeartbeat = -1;

    // Transient bearer token for the next remote connect (#49 Phase 3).
    // sha256 hex of the password the user typed in the prompt. Not
    // persisted: set on browse-click or Manual-tab connect, consumed
    // by Application's onDeviceChanged callback, then cleared.
    std::string m_remoteAuthToken;

    // Buffer configuration (para economizar memória, especialmente em ARM)
    // Default video buffer raised from 10 to 15 frames (~250ms at 60fps)
    // to give a bit more cushion against client/network jitter — small
    // frame loss showed up under load even on capable hardware. Audio
    // bumped to match. Still overridable from config.json.
    size_t m_streamingMaxVideoBufferSize = 15;     // Máximo de frames no buffer de vídeo (1-50)
    size_t m_streamingMaxAudioBufferSize = 30;     // Máximo de chunks no buffer de áudio (5-100)
    int64_t m_streamingMaxBufferTimeSeconds = 5;   // Tempo máximo de buffer em segundos (1-30)
    size_t m_streamingAVIOBufferSize = 256 * 1024; // 256KB para buffer AVIO do FFmpeg (64KB-1MB)

    std::function<void(bool)> m_onStreamingStartStop;
    std::function<void(uint16_t)> m_onStreamingPortChanged;
    std::function<void(uint32_t)> m_onStreamingWidthChanged;
    std::function<void(uint32_t)> m_onStreamingHeightChanged;
    std::function<void(uint32_t)> m_onStreamingFpsChanged;
    std::function<void(uint32_t)> m_onStreamingBitrateChanged;
    std::function<void(uint32_t)> m_onStreamingAudioBitrateChanged;
    std::function<void(const std::string &)> m_onStreamingVideoCodecChanged;
    std::function<void(const std::string &)> m_onStreamingAudioCodecChanged;
    std::function<void(const std::string &)> m_onStreamingH264PresetChanged;
    std::function<void(const std::string &)> m_onStreamingH265PresetChanged;
    std::function<void(const std::string &)> m_onStreamingH265ProfileChanged;
    std::function<void(const std::string &)> m_onStreamingH265LevelChanged;
    std::function<void(int)> m_onStreamingVP8SpeedChanged;
    std::function<void(int)> m_onStreamingVP9SpeedChanged;
    std::function<void(int)> m_onStreamingHardwareEncoderChanged;
    std::function<void(const std::string &)> m_onStreamingNvencPresetChanged;
    std::function<void(const std::string &)> m_onStreamingVaapiRcModeChanged;
    std::function<void(const std::string &)> m_onStreamingQsvPresetChanged;
    std::function<void(const std::string &)> m_onStreamingAmfQualityChanged;
    std::function<void(const std::string &)> m_onRemoteInterpolationChanged;
    std::function<void(size_t)> m_onStreamingMaxVideoBufferSizeChanged;
    std::function<void(size_t)> m_onStreamingMaxAudioBufferSizeChanged;
    std::function<void(int64_t)> m_onStreamingMaxBufferTimeSecondsChanged;
    std::function<void(size_t)> m_onStreamingAVIOBufferSizeChanged;

    // Recording state
    bool m_recordingActive = false;
    uint64_t m_recordingDurationUs = 0;
    uint64_t m_recordingFileSize = 0;
    std::string m_recordingFilename;
    uint32_t m_recordingWidth = 1920;
    uint32_t m_recordingHeight = 1080;
    uint32_t m_recordingFps = 60;
    uint32_t m_recordingBitrate = 8000000;
    uint32_t m_recordingAudioBitrate = 256000;
    std::string m_recordingVideoCodec = "h264";
    std::string m_recordingAudioCodec = "aac";
    std::string m_recordingH264Preset = "veryfast";
    std::string m_recordingH265Preset = "veryfast";
    std::string m_recordingH265Profile = "main";
    std::string m_recordingH265Level = "auto";
    int m_recordingVP8Speed = 12;
    int m_recordingVP9Speed = 6;
    std::string m_recordingContainer = "mp4";
    std::string m_recordingOutputPath = "recordings/";
    std::string m_recordingFilenameTemplate = "recording_%Y%m%d_%H%M%S";
    bool m_recordingIncludeAudio = true;
    // Mirrors the streaming side (#59). 0=Auto (try hardware,
    // fall back to libx264), 1=Software, 2=NVENC, 3=VAAPI, 4=QSV,
    // 5=AMF. Backend-specific presets live in separate fields.
    int         m_recordingHardwareEncoder = 0;
    std::string m_recordingNvencPreset = "p4";
    std::string m_recordingVaapiRcMode = "VBR"; // VBR is the better default for files
    std::string m_recordingQsvPreset   = "medium";
    std::string m_recordingAmfQuality  = "quality"; // recordings can afford the latency for visual quality

    // Recording callbacks
    std::function<void(bool)> m_onRecordingStartStop;
    std::function<void(uint32_t)> m_onRecordingWidthChanged;
    std::function<void(uint32_t)> m_onRecordingHeightChanged;
    std::function<void(uint32_t)> m_onRecordingFpsChanged;
    std::function<void(uint32_t)> m_onRecordingBitrateChanged;
    std::function<void(uint32_t)> m_onRecordingAudioBitrateChanged;
    std::function<void(const std::string&)> m_onRecordingVideoCodecChanged;
    std::function<void(const std::string&)> m_onRecordingAudioCodecChanged;
    std::function<void(const std::string&)> m_onRecordingH264PresetChanged;
    std::function<void(const std::string&)> m_onRecordingH265PresetChanged;
    std::function<void(const std::string&)> m_onRecordingH265ProfileChanged;
    std::function<void(const std::string&)> m_onRecordingH265LevelChanged;
    std::function<void(int)> m_onRecordingVP8SpeedChanged;
    std::function<void(int)> m_onRecordingVP9SpeedChanged;
    std::function<void(const std::string&)> m_onRecordingContainerChanged;
    std::function<void(const std::string&)> m_onRecordingOutputPathChanged;
    std::function<void(const std::string&)> m_onRecordingFilenameTemplateChanged;
    std::function<void(bool)> m_onRecordingIncludeAudioChanged;
    std::function<void(int)> m_onRecordingHardwareEncoderChanged;
    std::function<void(const std::string &)> m_onRecordingNvencPresetChanged;
    std::function<void(const std::string &)> m_onRecordingVaapiRcModeChanged;
    std::function<void(const std::string &)> m_onRecordingQsvPresetChanged;
    std::function<void(const std::string &)> m_onRecordingAmfQualityChanged;

    // Web Portal settings
    bool m_webPortalEnabled = true; // Habilitado por padrão
    bool m_webPortalHTTPSEnabled = false;
    std::string m_webPortalSSLCertPath = "ssl/server.crt";
    std::string m_webPortalSSLKeyPath = "ssl/server.key";
    std::string m_foundSSLCertPath;                                       // Caminho real do certificado encontrado (após busca)
    std::string m_foundSSLKeyPath;                                        // Caminho real da chave encontrada (após busca)
    std::string m_webPortalTitle = "RetroCapture Stream";                 // Título da página web
    std::string m_webPortalSubtitle = "Real-time video streaming"; // Subtítulo
    std::string m_webPortalImagePath = "logo.png";                        // Caminho da imagem para o título (padrão: logo.png)
    std::string m_webPortalBackgroundImagePath;                           // Caminho da imagem de fundo (opcional)

    // Textos editáveis dos cards
    std::string m_webPortalTextStreamInfo = "Stream Information";
    std::string m_webPortalTextQuickActions = "Quick Actions";
    std::string m_webPortalTextCompatibility = "Compatibilidade";
    std::string m_webPortalTextStatus = "Status";
    std::string m_webPortalTextCodec = "Codec";
    std::string m_webPortalTextResolution = "Resolution";
    std::string m_webPortalTextStreamUrl = "URL do Stream";
    std::string m_webPortalTextCopyUrl = "Copiar URL";
    std::string m_webPortalTextOpenNewTab = "Abrir em Nova Aba";
    std::string m_webPortalTextSupported = "Suportado";
    std::string m_webPortalTextFormat = "Formato";
    std::string m_webPortalTextCodecInfo = "Codec";
    std::string m_webPortalTextSupportedBrowsers = "Chrome, Firefox, Safari, Edge";
    std::string m_webPortalTextFormatInfo = "HLS (HTTP Live Streaming)";
    std::string m_webPortalTextCodecInfoValue = "H.264/AAC";
    std::string m_webPortalTextConnecting = "Conectando...";

    // Cores do portal baseadas no styleguide RetroCapture (RGBA, valores 0.0-1.0)
    // Primary - Retro Teal #0A7A83
    float m_webPortalColorPrimary[4] = {0.039f, 0.478f, 0.514f, 1.0f};
    // Primary Light - Mint Screen Glow #6FC4C0
    float m_webPortalColorPrimaryLight[4] = {0.435f, 0.769f, 0.753f, 1.0f};
    // Primary Dark - Deep Retro #0F3E42
    float m_webPortalColorPrimaryDark[4] = {0.059f, 0.243f, 0.259f, 1.0f};
    // Secondary - Cyan Oscilloscope #47B3CE
    float m_webPortalColorSecondary[4] = {0.278f, 0.702f, 0.808f, 1.0f};
    // Secondary Highlight - Phosphor Glow #C9F2E7
    float m_webPortalColorSecondaryHighlight[4] = {0.788f, 0.949f, 0.906f, 1.0f};
    // Dark Background #1D1F21
    float m_webPortalColorBackground[4] = {0.114f, 0.122f, 0.129f, 1.0f};
    // Text Light #F8F8F2
    float m_webPortalColorText[4] = {0.973f, 0.973f, 0.949f, 1.0f};
    // Card Header (usa Primary Dark)
    float m_webPortalColorCardHeader[4] = {0.059f, 0.243f, 0.259f, 1.0f};
    // Border (usa Primary com transparência)
    float m_webPortalColorBorder[4] = {0.039f, 0.478f, 0.514f, 0.5f};
    // Success #45D6A4
    float m_webPortalColorSuccess[4] = {0.271f, 0.839f, 0.643f, 1.0f};
    // Warning #F3C93E
    float m_webPortalColorWarning[4] = {0.953f, 0.788f, 0.243f, 1.0f};
    // Error #D9534F
    float m_webPortalColorDanger[4] = {0.851f, 0.325f, 0.310f, 1.0f};
    // Info #4CBCE6
    float m_webPortalColorInfo[4] = {0.298f, 0.737f, 0.902f, 1.0f};

    std::function<void(bool)> m_onWebPortalEnabledChanged;
    std::function<void(bool)> m_onWebPortalHTTPSChanged;
    std::function<void(const std::string &)> m_onWebPortalSSLCertPathChanged;
    std::function<void(const std::string &)> m_onWebPortalSSLKeyPathChanged;
    std::function<void(const std::string &)> m_onWebPortalTitleChanged;
    std::function<void(const std::string &)> m_onWebPortalSubtitleChanged;
    std::function<void(const std::string &)> m_onWebPortalImagePathChanged;
    std::function<void(const std::string &)> m_onWebPortalBackgroundImagePathChanged;
    std::function<void()> m_onWebPortalColorsChanged;
    std::function<void()> m_onWebPortalTextsChanged;
    std::function<void(bool)> m_onWebPortalStartStop;

    // Web Portal active state (independent from streaming)
    bool m_webPortalActive = false;
};
