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

// #160 — UIManager streaming settings grouped into a config struct (first of the
// config-struct groups). Storage consolidation: the per-field getters/setters are
// now thin wrappers over this, plus bulk get/setStreamingConfig() accessors.
struct StreamingConfig
{
    uint16_t    port            = 8080;
    uint32_t    width           = 640;
    uint32_t    height          = 480;
    uint32_t    fps             = 60;
    uint32_t    bitrate         = 8000;
    uint32_t    audioBitrate    = 256;
    std::string videoCodec      = "h264";
    std::string audioCodec      = "aac";
    std::string h264Preset      = "veryfast";
    std::string h265Preset      = "veryfast";
    std::string h265Profile     = "main";
    std::string h265Level       = "auto";
    int         vp8Speed        = 12;
    int         vp9Speed        = 6;
    int         hardwareEncoder = 0;       // 0 = Auto (MediaEncoder::HardwareEncoder::Auto)
    std::string nvencPreset     = "p4";
    std::string vaapiRcMode     = "CBR";
    std::string qsvPreset       = "veryfast";
    std::string amfQuality      = "speed";
    size_t      maxVideoBufferSize   = 15;
    size_t      maxAudioBufferSize   = 30;
    int64_t     maxBufferTimeSeconds = 5;
    size_t      avioBufferSize       = 256 * 1024;
};

// #160 — UIManager recording settings grouped into a config struct (group 2/N).
struct RecordingConfig
{
    uint32_t    width            = 1920;
    uint32_t    height           = 1080;
    uint32_t    fps              = 60;
    uint32_t    bitrate          = 8000000;
    uint32_t    audioBitrate     = 256000;
    std::string videoCodec       = "h264";
    std::string audioCodec       = "aac";
    std::string h264Preset       = "veryfast";
    std::string h265Preset       = "veryfast";
    std::string h265Profile      = "main";
    std::string h265Level        = "auto";
    int         vp8Speed         = 12;
    int         vp9Speed         = 6;
    std::string container        = "mp4";
    std::string outputPath       = "recordings/";
    std::string filenameTemplate = "recording_%Y%m%d_%H%M%S";
    bool        includeAudio     = true;
    int         hardwareEncoder  = 0;          // 0=Auto 1=SW 2=NVENC 3=VAAPI 4=QSV 5=AMF
    std::string nvencPreset      = "p4";
    std::string vaapiRcMode      = "VBR";
    std::string qsvPreset        = "medium";
    std::string amfQuality       = "quality";
};

// #160 — UIManager web-portal settings + theme strings grouped (group 3/N).
struct WebPortalConfig
{
    bool        enabled       = true;
    bool        httpsEnabled  = false;
    std::string sslCertPath   = "ssl/server.crt";
    std::string sslKeyPath    = "ssl/server.key";
    std::string title         = "RetroCapture Stream";
    std::string subtitle      = "Real-time video streaming";
    std::string imagePath     = "logo.png";
    std::string textStreamInfo        = "Stream Information";
    std::string textQuickActions      = "Quick Actions";
    std::string textCompatibility     = "Compatibilidade";
    std::string textStatus            = "Status";
    std::string textCodec             = "Codec";
    std::string textResolution        = "Resolution";
    std::string textStreamUrl         = "URL do Stream";
    std::string textCopyUrl           = "Copiar URL";
    std::string textOpenNewTab        = "Abrir em Nova Aba";
    std::string textSupported         = "Suportado";
    std::string textFormat            = "Formato";
    std::string textCodecInfo         = "Codec";
    std::string textSupportedBrowsers = "Chrome, Firefox, Safari, Edge";
    std::string textFormatInfo        = "HLS (HTTP Live Streaming)";
    std::string textCodecInfoValue    = "H.264/AAC";
    std::string textConnecting        = "Conectando...";
};

// #160 — UIManager directory publish settings + live status grouped (group 4/N).
struct DirectoryState
{
    bool        publishEnabled      = false;
    std::string url                 = "https://directory.retrocapture.com";
    bool        insecureSkipVerify  = false;
    std::string streamName          = "";
    std::string hostNickname        = "";
    std::string password            = "";       // optional; empty = no password
    std::string endpointMode        = "direct"; // direct | tunnel-cloudflare | custom
    std::string customEndpoint      = "";
    std::string tunnelMode          = "quick";  // quick | named
    std::string namedTunnelId       = "";
    std::string namedTunnelHostname = "";
    bool        privacyAcked        = false;    // sticky once the user accepts the warning
    // Live status mirrored from Application/DirectoryClient; UI just reads.
    std::string statusText          = "Idle";
    std::string streamId            = "";
    uint64_t    registerOk          = 0;
    uint64_t    registerFail        = 0;
    uint64_t    heartbeatOk         = 0;
    uint64_t    heartbeatFail       = 0;
    uint64_t    patchOk             = 0;
    uint64_t    patchFail           = 0;
    int64_t     secondsSinceLastHeartbeat = -1;
};

// #160 — UIManager remote-source playback status + client settings grouped (group 5/N).
struct RemoteState
{
    bool        hostLikelyOffline     = false;
    bool        receivingFrames       = false;
    bool        initialConnectFailing = false;
    uint32_t    upstreamClientCount   = 0;
    std::string interpolation         = "linear";
    float       audioVolume           = 1.0f;
    bool        audioMuted            = false;
};

// #160 — UIManager chat settings grouped (group 6/N, final grouping group).
struct ChatConfig
{
    bool        overlayVisible = true;
    std::string baseUrl        = "https://chat.retrocapture.com";
    std::string nickname       = "";
};

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
        AVFoundation = 4, // AVFoundation (macOS)
        Screen = 5,       // Desktop / window / region screen capture (#107)
        Test = 6          // Synthetic test-pattern source for the smoke-test (#149)
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
    bool getDirectoryPublishEnabled() const          { return m_directoryState.publishEnabled; }
    void setDirectoryPublishEnabled(bool v)          { m_directoryState.publishEnabled = v; }
    const std::string &getDirectoryUrl() const       { return m_directoryState.url; }
    void setDirectoryUrl(const std::string &v)       { m_directoryState.url = v; }
    bool getDirectoryInsecureSkipVerify() const      { return m_directoryState.insecureSkipVerify; }
    void setDirectoryInsecureSkipVerify(bool v)      { m_directoryState.insecureSkipVerify = v; }
    // #84 — Chat service base URL (ws:// or wss://). Editable under
    // Configurations → Streaming → Advanced, mirroring the directory
    // URL field. Application reads this every frame and reconfigures
    // the ChatClient when it changes (cheap no-op when unchanged).
    const std::string &getChatBaseUrl() const        { return m_chatConfig.baseUrl; }
    void setChatBaseUrl(const std::string &v)        { m_chatConfig.baseUrl = v; }
    // #84 — Persistent chat nickname. Shared between host and viewer
    // modes (you're "geldo" regardless of which side you're on); the
    // OSD chat panel writes here when the user clicks Apply. Empty
    // means "use a fallback": Application falls back to the directory
    // host nickname when publishing, or sends an empty hello (server
    // generates anon-<rand>) when viewing.
    const std::string &getChatNickname() const       { return m_chatConfig.nickname; }
    void setChatNickname(const std::string &v)       { m_chatConfig.nickname = v; }
    // #160 — bulk access to the chat settings group (per-field accessors are
    // thin wrappers over the same struct).
    const ChatConfig &getChatConfig() const { return m_chatConfig; }
    void setChatConfig(const ChatConfig &cfg) { m_chatConfig = cfg; }
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
    // #68 follow-up — auto-hide the quick-actions OSD after mouse idle.
    bool getQuickActionsAutoHide() const             { return m_quickActionsAutoHide; }
    void setQuickActionsAutoHide(bool v)             { m_quickActionsAutoHide = v; }
    // #86 — system tray / background operation preferences.
    bool getTrayEnabled() const                      { return m_trayEnabled; }
    void setTrayEnabled(bool v)                      { m_trayEnabled = v; }
    bool getTrayMinimizeOnClose() const              { return m_trayMinimizeOnClose; }
    void setTrayMinimizeOnClose(bool v)              { m_trayMinimizeOnClose = v; }
    bool getTrayStartMinimized() const               { return m_trayStartMinimized; }
    void setTrayStartMinimized(bool v)               { m_trayStartMinimized = v; }
    bool getTrayNotifications() const                { return m_trayNotifications; }
    void setTrayNotifications(bool v)                { m_trayNotifications = v; }
    const std::string &getDirectoryStreamName() const { return m_directoryState.streamName; }
    void setDirectoryStreamName(const std::string &v) { m_directoryState.streamName = v; }
    // #84 — As of the profile unification, the directory's "host
    // nickname" is derived from the chat Profile (m_chatConfig.nickname).
    // The setter is still here for ConfigJSON round-trips of legacy
    // configs that have a separate hostNickname field; on first
    // load we mirror it into m_chatConfig.nickname when the latter is
    // empty, then the chat profile owns the value going forward.
    const std::string &getDirectoryHostNickname() const
    {
        return m_chatConfig.nickname.empty() ? m_directoryState.hostNickname : m_chatConfig.nickname;
    }
    void setDirectoryHostNickname(const std::string &v) { m_directoryState.hostNickname = v; }
    const std::string &getDirectoryPassword() const  { return m_directoryState.password; }
    void setDirectoryPassword(const std::string &v)  { m_directoryState.password = v; }
    const std::string &getDirectoryEndpointMode() const { return m_directoryState.endpointMode; }
    void setDirectoryEndpointMode(const std::string &v) { m_directoryState.endpointMode = v; }
    const std::string &getDirectoryCustomEndpoint() const { return m_directoryState.customEndpoint; }
    void setDirectoryCustomEndpoint(const std::string &v) { m_directoryState.customEndpoint = v; }
    // Phase 2.5c (#60): Cloudflare Tunnel sub-mode + Named-tunnel state.
    // tunnelMode is "quick" (default) or "named". When "named", the
    // tunnel id + hostname identify which existing tunnel cloudflared
    // should run, and the directory entry's URL points at the user's
    // own hostname instead of a fresh trycloudflare.com one.
    const std::string &getDirectoryTunnelMode() const { return m_directoryState.tunnelMode; }
    void setDirectoryTunnelMode(const std::string &v) { m_directoryState.tunnelMode = v; }
    const std::string &getDirectoryNamedTunnelId() const { return m_directoryState.namedTunnelId; }
    void setDirectoryNamedTunnelId(const std::string &v) { m_directoryState.namedTunnelId = v; }
    const std::string &getDirectoryNamedTunnelHostname() const { return m_directoryState.namedTunnelHostname; }
    void setDirectoryNamedTunnelHostname(const std::string &v) { m_directoryState.namedTunnelHostname = v; }
    bool getDirectoryPrivacyAcked() const            { return m_directoryState.privacyAcked; }
    void setDirectoryPrivacyAcked(bool v)            { m_directoryState.privacyAcked = v; }
    const std::string &getDirectoryStatusText() const { return m_directoryState.statusText; }
    void setDirectoryStatusText(const std::string &v) { m_directoryState.statusText = v; }
    // #84 — Currently-published directory streamId (empty when not
    // Active). Application mirrors DirectoryClient::getStreamId() here
    // every frame so APIController can embed it in /meta for the chat
    // panel on remote clients.
    const std::string &getDirectoryStreamId() const   { return m_directoryState.streamId; }
    void setDirectoryStreamId(const std::string &v)   { m_directoryState.streamId = v; }
    const std::string &getRemoteAuthToken() const  { return m_remoteAuthToken; }
    void setRemoteAuthToken(const std::string &v)  { m_remoteAuthToken = v; }

    // Directory telemetry getters/setters (#49 Phase 5).
    uint64_t getDirectoryRegisterOk()    const { return m_directoryState.registerOk; }
    uint64_t getDirectoryRegisterFail()  const { return m_directoryState.registerFail; }
    uint64_t getDirectoryHeartbeatOk()   const { return m_directoryState.heartbeatOk; }
    uint64_t getDirectoryHeartbeatFail() const { return m_directoryState.heartbeatFail; }
    uint64_t getDirectoryPatchOk()       const { return m_directoryState.patchOk; }
    uint64_t getDirectoryPatchFail()     const { return m_directoryState.patchFail; }
    int64_t  getDirectorySecondsSinceLastHeartbeat() const { return m_directoryState.secondsSinceLastHeartbeat; }
    // #160 — bulk access to the directory settings+status group (per-field
    // accessors are thin wrappers over the same struct).
    const DirectoryState &getDirectoryState() const { return m_directoryState; }
    void setDirectoryState(const DirectoryState &st) { m_directoryState = st; }
    void setDirectoryStats(uint64_t regOk, uint64_t regFail,
                           uint64_t hbOk, uint64_t hbFail,
                           uint64_t patchOk, uint64_t patchFail,
                           int64_t secondsSinceLastHeartbeat)
    {
        m_directoryState.registerOk    = regOk;
        m_directoryState.registerFail  = regFail;
        m_directoryState.heartbeatOk   = hbOk;
        m_directoryState.heartbeatFail = hbFail;
        m_directoryState.patchOk       = patchOk;
        m_directoryState.patchFail     = patchFail;
        m_directoryState.secondsSinceLastHeartbeat = secondsSinceLastHeartbeat;
    }
    void setStreamingPort(uint16_t port);
    void setStreamingWidth(uint32_t width) { m_streamingConfig.width = width; }
    void setStreamingHeight(uint32_t height) { m_streamingConfig.height = height; }
    void setStreamingFps(uint32_t fps) { m_streamingConfig.fps = fps; }
    void setStreamingBitrate(uint32_t bitrate) { m_streamingConfig.bitrate = bitrate; }
    void setStreamingAudioBitrate(uint32_t bitrate) { m_streamingConfig.audioBitrate = bitrate; }
    void setStreamingVideoCodec(const std::string &codec) { m_streamingConfig.videoCodec = codec; }
    void setStreamingAudioCodec(const std::string &codec) { m_streamingConfig.audioCodec = codec; }
    void setStreamingH264Preset(const std::string &preset) { m_streamingConfig.h264Preset = preset; }
    void setStreamingH265Preset(const std::string &preset) { m_streamingConfig.h265Preset = preset; }
    void setStreamingH265Profile(const std::string &profile) { m_streamingConfig.h265Profile = profile; }
    void setStreamingH265Level(const std::string &level) { m_streamingConfig.h265Level = level; }
    void setStreamingVP8Speed(int speed) { m_streamingConfig.vp8Speed = speed; }
    void setStreamingVP9Speed(int speed) { m_streamingConfig.vp9Speed = speed; }
    // Hardware encoder selection — uses int so we don't have to pull
    // MediaEncoder.h into the UI layer. Stored as int matching the
    // MediaEncoder::HardwareEncoder enum values (Auto=0, Software=1,
    // NVENC=2, VAAPI=3, QSV=4, AMF=5).
    void setStreamingHardwareEncoder(int v) { m_streamingConfig.hardwareEncoder = v; }

    // Buffer settings
    void setStreamingMaxVideoBufferSize(size_t size) { m_streamingConfig.maxVideoBufferSize = size; }
    void setStreamingMaxAudioBufferSize(size_t size) { m_streamingConfig.maxAudioBufferSize = size; }
    void setStreamingMaxBufferTimeSeconds(int64_t seconds) { m_streamingConfig.maxBufferTimeSeconds = seconds; }
    void setStreamingAVIOBufferSize(size_t size) { m_streamingConfig.avioBufferSize = size; }

    size_t getStreamingMaxVideoBufferSize() const { return m_streamingConfig.maxVideoBufferSize; }
    size_t getStreamingMaxAudioBufferSize() const { return m_streamingConfig.maxAudioBufferSize; }
    int64_t getStreamingMaxBufferTimeSeconds() const { return m_streamingConfig.maxBufferTimeSeconds; }
    size_t getStreamingAVIOBufferSize() const { return m_streamingConfig.avioBufferSize; }

    // Streaming info getters (public)
    uint16_t getStreamingPort() const { return m_streamingConfig.port; }
    uint32_t getStreamingWidth() const { return m_streamingConfig.width; }
    uint32_t getStreamingHeight() const { return m_streamingConfig.height; }
    uint32_t getStreamingFps() const { return m_streamingConfig.fps; }
    uint32_t getStreamingBitrate() const { return m_streamingConfig.bitrate; }
    uint32_t getStreamingAudioBitrate() const { return m_streamingConfig.audioBitrate; }
    std::string getStreamingVideoCodec() const { return m_streamingConfig.videoCodec; }
    std::string getStreamingAudioCodec() const { return m_streamingConfig.audioCodec; }
    std::string getStreamingH264Preset() const { return m_streamingConfig.h264Preset; }
    std::string getStreamingH265Preset() const { return m_streamingConfig.h265Preset; }
    std::string getStreamingH265Profile() const { return m_streamingConfig.h265Profile; }
    std::string getStreamingH265Level() const { return m_streamingConfig.h265Level; }
    int getStreamingVP8Speed() const { return m_streamingConfig.vp8Speed; }
    int getStreamingVP9Speed() const { return m_streamingConfig.vp9Speed; }
    int getStreamingHardwareEncoder() const { return m_streamingConfig.hardwareEncoder; }
    std::string getStreamingNvencPreset() const { return m_streamingConfig.nvencPreset; }
    std::string getStreamingVaapiRcMode() const { return m_streamingConfig.vaapiRcMode; }
    std::string getStreamingQsvPreset()   const { return m_streamingConfig.qsvPreset; }
    std::string getStreamingAmfQuality()  const { return m_streamingConfig.amfQuality; }

    // #160 — bulk access to the whole streaming-settings group. New code should
    // prefer these; the per-field accessors above are thin wrappers over the same
    // struct and will be migrated/removed in follow-up PRs.
    const StreamingConfig &getStreamingConfig() const { return m_streamingConfig; }
    void setStreamingConfig(const StreamingConfig &cfg) { m_streamingConfig = cfg; }

    std::string getRemoteInterpolation() const { return m_remoteState.interpolation; }
    // #160 — bulk access to the remote-source state group (per-field accessors
    // are thin wrappers over the same struct).
    const RemoteState &getRemoteState() const { return m_remoteState; }
    void setRemoteState(const RemoteState &st) { m_remoteState = st; }
    // #77 client-side volume for the incoming remote audio stream.
    float getRemoteAudioVolume() const { return m_remoteState.audioVolume; }
    bool  getRemoteAudioMuted()  const { return m_remoteState.audioMuted; }
    // #107 screen-capture region crop (target pixels; 0,0,0,0 = full).
    uint32_t getScreenRegionX() const { return m_screenRegionX; }
    uint32_t getScreenRegionY() const { return m_screenRegionY; }
    uint32_t getScreenRegionW() const { return m_screenRegionW; }
    uint32_t getScreenRegionH() const { return m_screenRegionH; }
    void triggerScreenRegionChange(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    void setOnScreenRegionChanged(std::function<void(uint32_t, uint32_t, uint32_t, uint32_t)> cb)
    {
        m_onScreenRegionChanged = cb;
    }
    // Apply a region to the live capture WITHOUT persisting or updating
    // the stored values — used by the visual region selector to show the
    // full (uncropped) frame while picking. saveConfig happens only on
    // the final triggerScreenRegionChange().
    void applyScreenRegionLive(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
    {
        if (m_onScreenRegionChanged) m_onScreenRegionChanged(x, y, w, h);
    }
    // Live capture GL texture, published each frame by Application so the
    // region selector can draw the current frame. 0 == none.
    void setCaptureTexture(unsigned int tex, uint32_t w, uint32_t h)
    {
        m_captureTex = tex; m_captureTexW = w; m_captureTexH = h;
    }
    unsigned int getCaptureTextureId() const { return m_captureTex; }
    uint32_t getCaptureTextureWidth() const  { return m_captureTexW; }
    uint32_t getCaptureTextureHeight() const { return m_captureTexH; }

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
    // #77 — volume in [0,1]; mute is a separate latch that overrides
    // the slider value (so unmuting restores the last level). Both push
    // the effective gain (muted ? 0 : volume) through the callback.
    void triggerRemoteAudioVolumeChange(float volume);
    void triggerRemoteAudioMuteChange(bool muted);
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
    bool getRemoteHostLikelyOffline() const { return m_remoteState.hostLikelyOffline; }
    void setRemoteHostLikelyOffline(bool v) { m_remoteState.hostLikelyOffline = v; }
    // 'Are we decoding frames right now' — distinct from
    // captureWidth > 0, which stays at the last seen value after
    // the stream drops. Mirrored by Application from
    // VideoCaptureRemote::isReceivingFrames() every frame.
    bool getRemoteReceivingFrames() const { return m_remoteState.receivingFrames; }
    void setRemoteReceivingFrames(bool v) { m_remoteState.receivingFrames = v; }
    // First-connect-failing mirror: true while a brand-new connection
    // (no frame ever received yet) has failed a few times. Mirrored
    // from VideoCaptureRemote::isInitialConnectFailing() every frame.
    bool getRemoteInitialConnectFailing() const { return m_remoteState.initialConnectFailing; }
    void setRemoteInitialConnectFailing(bool v) { m_remoteState.initialConnectFailing = v; }

    // Host's current viewer count, parsed from /meta when we're in
    // client mode (#68). Application piggybacks the
    // RemoteMetaSync::Snapshot callback to keep this fresh. 0 when
    // we're not a client or the host's /meta predates this field.
    uint32_t getRemoteUpstreamClientCount() const { return m_remoteState.upstreamClientCount; }
    void setRemoteUpstreamClientCount(uint32_t v) { m_remoteState.upstreamClientCount = v; }
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
    // #77 — receives the effective linear gain (muted ? 0 : volume).
    void setOnRemoteAudioVolumeChanged   (std::function<void(float)> cb) { m_onRemoteAudioVolumeChanged = cb; }

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
    void setRecordingWidth(uint32_t width) { m_recordingConfig.width = width; }
    void setRecordingHeight(uint32_t height) { m_recordingConfig.height = height; }
    void setRecordingFps(uint32_t fps) { m_recordingConfig.fps = fps; }
    void setRecordingBitrate(uint32_t bitrate) { m_recordingConfig.bitrate = bitrate; }
    void setRecordingAudioBitrate(uint32_t bitrate) { m_recordingConfig.audioBitrate = bitrate; }
    void setRecordingVideoCodec(const std::string& codec) { m_recordingConfig.videoCodec = codec; }
    void setRecordingAudioCodec(const std::string& codec) { m_recordingConfig.audioCodec = codec; }
    void setRecordingH264Preset(const std::string& preset) { m_recordingConfig.h264Preset = preset; }
    void setRecordingH265Preset(const std::string& preset) { m_recordingConfig.h265Preset = preset; }
    void setRecordingH265Profile(const std::string& profile) { m_recordingConfig.h265Profile = profile; }
    void setRecordingH265Level(const std::string& level) { m_recordingConfig.h265Level = level; }
    void setRecordingVP8Speed(int speed) { m_recordingConfig.vp8Speed = speed; }
    void setRecordingVP9Speed(int speed) { m_recordingConfig.vp9Speed = speed; }
    void setRecordingContainer(const std::string& container) { m_recordingConfig.container = container; }
    void setRecordingOutputPath(const std::string& path) { m_recordingConfig.outputPath = path; }
    void setRecordingFilenameTemplate(const std::string& template_) { m_recordingConfig.filenameTemplate = template_; }
    void setRecordingIncludeAudio(bool include) { m_recordingConfig.includeAudio = include; }

    // Hardware encoder selection for recording (#59). Same int-based
    // encoding as the streaming side so we don't have to pull
    // MediaEncoder.h into the UI layer (0=Auto, 1=Software,
    // 2=NVENC, 3=VAAPI, 4=QSV, 5=AMF). Backend-specific preset
    // strings live in separate fields per backend, mirroring the
    // streaming layout exactly so the same UI block can render both.
    void setRecordingHardwareEncoder(int v)           { m_recordingConfig.hardwareEncoder = v; }
    void setRecordingNvencPreset(const std::string &v){ m_recordingConfig.nvencPreset = v; }
    void setRecordingVaapiRcMode(const std::string &v){ m_recordingConfig.vaapiRcMode = v; }
    void setRecordingQsvPreset(const std::string &v)  { m_recordingConfig.qsvPreset = v; }
    void setRecordingAmfQuality(const std::string &v) { m_recordingConfig.amfQuality = v; }

    // Recording info getters (public)
    bool getRecordingActive() const { return m_recordingActive; }
    uint64_t getRecordingDurationUs() const { return m_recordingDurationUs; }
    uint64_t getRecordingFileSize() const { return m_recordingFileSize; }
    std::string getRecordingFilename() const { return m_recordingFilename; }
    uint32_t getRecordingWidth() const { return m_recordingConfig.width; }
    uint32_t getRecordingHeight() const { return m_recordingConfig.height; }
    uint32_t getRecordingFps() const { return m_recordingConfig.fps; }
    uint32_t getRecordingBitrate() const { return m_recordingConfig.bitrate; }
    uint32_t getRecordingAudioBitrate() const { return m_recordingConfig.audioBitrate; }
    std::string getRecordingVideoCodec() const { return m_recordingConfig.videoCodec; }
    std::string getRecordingAudioCodec() const { return m_recordingConfig.audioCodec; }
    std::string getRecordingH264Preset() const { return m_recordingConfig.h264Preset; }
    std::string getRecordingH265Preset() const { return m_recordingConfig.h265Preset; }
    std::string getRecordingH265Profile() const { return m_recordingConfig.h265Profile; }
    std::string getRecordingH265Level() const { return m_recordingConfig.h265Level; }
    int getRecordingVP8Speed() const { return m_recordingConfig.vp8Speed; }
    int getRecordingVP9Speed() const { return m_recordingConfig.vp9Speed; }
    std::string getRecordingContainer() const { return m_recordingConfig.container; }
    std::string getRecordingOutputPath() const { return m_recordingConfig.outputPath; }
    std::string getRecordingFilenameTemplate() const { return m_recordingConfig.filenameTemplate; }
    bool getRecordingIncludeAudio() const { return m_recordingConfig.includeAudio; }
    int  getRecordingHardwareEncoder() const         { return m_recordingConfig.hardwareEncoder; }
    std::string getRecordingNvencPreset() const      { return m_recordingConfig.nvencPreset; }
    std::string getRecordingVaapiRcMode() const      { return m_recordingConfig.vaapiRcMode; }
    std::string getRecordingQsvPreset()   const      { return m_recordingConfig.qsvPreset; }
    std::string getRecordingAmfQuality()  const      { return m_recordingConfig.amfQuality; }

    // #160 — bulk access to the whole recording-settings group (per-field
    // accessors above are thin wrappers over the same struct).
    const RecordingConfig &getRecordingConfig() const { return m_recordingConfig; }
    void setRecordingConfig(const RecordingConfig &cfg) { m_recordingConfig = cfg; }

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
    void setWebPortalEnabled(bool enabled) { m_webPortalConfig.enabled = enabled; }
    void setWebPortalHTTPSEnabled(bool enabled) { m_webPortalConfig.httpsEnabled = enabled; }
    void setWebPortalSSLCertPath(const std::string &path) { m_webPortalConfig.sslCertPath = path; }
    void setWebPortalSSLKeyPath(const std::string &path) { m_webPortalConfig.sslKeyPath = path; }
    void setFoundSSLCertificatePath(const std::string &path) { m_foundSSLCertPath = path; }
    void setFoundSSLKeyPath(const std::string &path) { m_foundSSLKeyPath = path; }
    void setWebPortalTitle(const std::string &title) { m_webPortalConfig.title = title; }
    void setWebPortalImagePath(const std::string &path) { m_webPortalConfig.imagePath = path; }
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

    bool getWebPortalEnabled() const { return m_webPortalConfig.enabled; }
    bool getWebPortalHTTPSEnabled() const { return m_webPortalConfig.httpsEnabled; }
    std::string getWebPortalSSLCertPath() const { return m_webPortalConfig.sslCertPath; }
    std::string getWebPortalSSLKeyPath() const { return m_webPortalConfig.sslKeyPath; }
    std::string getFoundSSLCertificatePath() const { return m_foundSSLCertPath; }
    std::string getFoundSSLKeyPath() const { return m_foundSSLKeyPath; }
    std::string getWebPortalTitle() const { return m_webPortalConfig.title; }
    std::string getWebPortalSubtitle() const { return m_webPortalConfig.subtitle; }
    std::string getWebPortalImagePath() const { return m_webPortalConfig.imagePath; }
    // #160 — bulk access to the web-portal settings group (per-field accessors
    // are thin wrappers over the same struct).
    const WebPortalConfig &getWebPortalConfig() const { return m_webPortalConfig; }
    void setWebPortalConfig(const WebPortalConfig &cfg) { m_webPortalConfig = cfg; }
    std::string getWebPortalBackgroundImagePath() const { return m_webPortalBackgroundImagePath; }

    // Getters para textos editáveis
    std::string getWebPortalTextStreamInfo() const { return m_webPortalConfig.textStreamInfo; }
    std::string getWebPortalTextQuickActions() const { return m_webPortalConfig.textQuickActions; }
    std::string getWebPortalTextCompatibility() const { return m_webPortalConfig.textCompatibility; }
    std::string getWebPortalTextStatus() const { return m_webPortalConfig.textStatus; }
    std::string getWebPortalTextCodec() const { return m_webPortalConfig.textCodec; }
    std::string getWebPortalTextResolution() const { return m_webPortalConfig.textResolution; }
    std::string getWebPortalTextStreamUrl() const { return m_webPortalConfig.textStreamUrl; }
    std::string getWebPortalTextCopyUrl() const { return m_webPortalConfig.textCopyUrl; }
    std::string getWebPortalTextOpenNewTab() const { return m_webPortalConfig.textOpenNewTab; }
    std::string getWebPortalTextSupported() const { return m_webPortalConfig.textSupported; }
    std::string getWebPortalTextFormat() const { return m_webPortalConfig.textFormat; }
    std::string getWebPortalTextCodecInfo() const { return m_webPortalConfig.textCodecInfo; }
    std::string getWebPortalTextSupportedBrowsers() const { return m_webPortalConfig.textSupportedBrowsers; }
    std::string getWebPortalTextFormatInfo() const { return m_webPortalConfig.textFormatInfo; }
    std::string getWebPortalTextCodecInfoValue() const { return m_webPortalConfig.textCodecInfoValue; }
    std::string getWebPortalTextConnecting() const { return m_webPortalConfig.textConnecting; }

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
    std::string &getWebPortalTextStreamInfoEditable() { return m_webPortalConfig.textStreamInfo; }
    std::string &getWebPortalTextQuickActionsEditable() { return m_webPortalConfig.textQuickActions; }
    std::string &getWebPortalTextCompatibilityEditable() { return m_webPortalConfig.textCompatibility; }
    std::string &getWebPortalTextStatusEditable() { return m_webPortalConfig.textStatus; }
    std::string &getWebPortalTextCodecEditable() { return m_webPortalConfig.textCodec; }
    std::string &getWebPortalTextResolutionEditable() { return m_webPortalConfig.textResolution; }
    std::string &getWebPortalTextStreamUrlEditable() { return m_webPortalConfig.textStreamUrl; }
    std::string &getWebPortalTextCopyUrlEditable() { return m_webPortalConfig.textCopyUrl; }
    std::string &getWebPortalTextOpenNewTabEditable() { return m_webPortalConfig.textOpenNewTab; }
    std::string &getWebPortalTextSupportedEditable() { return m_webPortalConfig.textSupported; }
    std::string &getWebPortalTextFormatEditable() { return m_webPortalConfig.textFormat; }
    std::string &getWebPortalTextCodecInfoEditable() { return m_webPortalConfig.textCodecInfo; }
    std::string &getWebPortalTextSupportedBrowsersEditable() { return m_webPortalConfig.textSupportedBrowsers; }
    std::string &getWebPortalTextFormatInfoEditable() { return m_webPortalConfig.textFormatInfo; }
    std::string &getWebPortalTextCodecInfoValueEditable() { return m_webPortalConfig.textCodecInfoValue; }
    std::string &getWebPortalTextConnectingEditable() { return m_webPortalConfig.textConnecting; }

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
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
    std::unique_ptr<class UIConfigurationVirtualCamera> m_virtcamWindow;
#endif
    std::unique_ptr<class UIConfigurationRecording> m_recordingWindow;
    std::unique_ptr<class UIConfigurationWebPortal> m_webPortalWindow;
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
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
    // Auto-hide the quick-actions overlay after the mouse goes idle, and
    // reveal it again on the next movement. Default on; toggled in
    // Preferences and persisted.
    bool m_quickActionsAutoHide = true;
    // #160 — chat settings grouped (see ChatConfig above).
    ChatConfig m_chatConfig;
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
    // #160 — remote-source playback state + settings grouped (see RemoteState above).
    RemoteState m_remoteState;
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
    // #160 — streaming settings grouped into one struct (see StreamingConfig above).
    StreamingConfig m_streamingConfig;
    // Per-backend quality / preset values — the Streaming-tab UI shows
    // whichever combo matches the currently selected hardware encoder.
    // Stored separately so switching encoders preserves each backend's
    // previously chosen value rather than collapsing them onto one
    // shared string whose meaning would shift mid-flight.

    // Client-side interpolation mode for Remote-source playback. Picks
    // how each display refresh resolves the time between two consecutive
    // stream frames (see VideoCaptureRemote::captureLatestFrame):
    //   "linear"  — LERP between prev and next (smooth motion, ghosting)
    //   "nearest" — show the closer frame (clean image, 3:2 stutter)
    //   "off"     — strict PTS gate, hold prev until next is due
    // #77 client-side remote audio volume. m_remoteState.audioVolume holds the
    // slider level [0,1]; m_remoteState.audioMuted overrides it to 0 while
    // preserving the level so unmuting restores it.
    // #107 screen-capture region crop, target pixels (0,0,0,0 = full target).
    uint32_t m_screenRegionX = 0, m_screenRegionY = 0, m_screenRegionW = 0, m_screenRegionH = 0;
    // Live capture texture published by Application for the region selector.
    unsigned int m_captureTex  = 0;
    uint32_t     m_captureTexW = 0;
    uint32_t     m_captureTexH = 0;
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
    // #160 — directory publish/tunnel settings + status grouped (see DirectoryState above).
    DirectoryState m_directoryState;
    // #84 — Chat URL. Production default; --chat-url overrides at
    // launch; the Streaming → Advanced field overrides at runtime.
    // Accepts https://, http://, wss://, ws:// — ChatClient
    // normalizes the scheme when building REST vs WS endpoints.
    // #84 — Persistent chat display name. Default empty; OSD chat
    // panel's Apply button writes here + saveConfig.
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
    // Preferences (#45 placeholder + window restructure)
    std::string m_language                = "en";    // "en" | "pt"
    bool        m_startFullscreen         = false;
    // #86 system tray. Default trayEnabled=true (the backend itself
    // reports unsupported and falls back cleanly when there's no
    // tray host); minimizeOnClose=true makes the X button background
    // the app instead of quitting.
    bool        m_trayEnabled             = true;
    bool        m_trayMinimizeOnClose     = true;
    bool        m_trayStartMinimized      = false;
    bool        m_trayNotifications       = false;
    // Phase 2.5c (#60): Cloudflare Tunnel sub-mode + Named-tunnel state.

    // Per-session telemetry counters mirrored from DirectoryClient
    // (#49 Phase 5). Application writes each frame; UI reads.

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
    std::function<void(float)> m_onRemoteAudioVolumeChanged;
    std::function<void(uint32_t, uint32_t, uint32_t, uint32_t)> m_onScreenRegionChanged;
    std::function<void(size_t)> m_onStreamingMaxVideoBufferSizeChanged;
    std::function<void(size_t)> m_onStreamingMaxAudioBufferSizeChanged;
    std::function<void(int64_t)> m_onStreamingMaxBufferTimeSecondsChanged;
    std::function<void(size_t)> m_onStreamingAVIOBufferSizeChanged;

    // Recording state
    bool m_recordingActive = false;
    uint64_t m_recordingDurationUs = 0;
    uint64_t m_recordingFileSize = 0;
    std::string m_recordingFilename;
    // #160 — recording settings grouped (see RecordingConfig above).
    RecordingConfig m_recordingConfig;
    // Mirrors the streaming side (#59). 0=Auto (try hardware,
    // fall back to libx264), 1=Software, 2=NVENC, 3=VAAPI, 4=QSV,
    // 5=AMF. Backend-specific presets live in separate fields.

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
    // #160 — web-portal settings grouped (see WebPortalConfig above).
    WebPortalConfig m_webPortalConfig;
    std::string m_foundSSLCertPath;                                       // Caminho real do certificado encontrado (após busca)
    std::string m_foundSSLKeyPath;                                        // Caminho real da chave encontrada (após busca)
    std::string m_webPortalBackgroundImagePath;                           // Caminho da imagem de fundo (opcional)

    // Textos editáveis dos cards

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
