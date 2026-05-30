#pragma once

#include <string>
#include <cstdint>
#include <cinttypes>
#include <deque>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>
#include <queue>
#include "../renderer/glad_loader.h"
#include "../utils/FilesystemCompat.h"

// Forward declarations for recording
struct RecordingSettings;
struct RecordingMetadata;

class IVideoCapture;
class IAudioCapture;
class WindowManager;
#ifdef USE_SDL2
class WindowManagerSDL;
#endif
class OpenGLRenderer;
class ShaderEngine;
class UIManager;
class FrameProcessor;
class StreamManager;
class HTTPTSStreamer;
class PBOManager;
class RecordingManager;

// Forward declaration for API
struct ShaderParameter;

// #86 — system tray abstraction (src/tray/ISystemTray.h)
namespace retrocapture { class ISystemTray; }

class Application
{
public:
    Application();
    ~Application();

    bool init();
    void run();
    void shutdown();

    // Configuração antes de init()
    void setShaderPath(const std::string &path) { m_shaderPath = path; }
    void setPresetPath(const std::string &path) { m_presetPath = path; }
    void setDevicePath(const std::string &path) { m_devicePath = path; }
    void setResolution(uint32_t width, uint32_t height)
    {
        m_captureWidth = width;
        m_captureHeight = height;
    }
    void setFramerate(uint32_t fps) { m_captureFps = fps; }
    void setWindowSize(uint32_t width, uint32_t height)
    {
        m_windowWidth = width;
        m_windowHeight = height;
    }
    void setFullscreen(bool fullscreen) { m_fullscreen = fullscreen; }
    void setMonitorIndex(int index) { m_monitorIndex = index; }
    void setMaintainAspect(bool maintain) { m_maintainAspect = maintain; }
    // #84 — Chat service base URL plumbing from --chat-url.
    void setChatBaseUrl(const std::string &url) { m_chatBaseUrl = url; }
    std::string getChatBaseUrl() const          { return m_chatBaseUrl; }
    void setBrightness(float brightness) { m_brightness = brightness; }
    void setContrast(float contrast) { m_contrast = contrast; }
    void setTextureFilterLinear(bool linear) { m_textureFilterLinear = linear; }
    bool getTextureFilterLinear() const { return m_textureFilterLinear; }
    
    // Configurar resolução de saída (0 = automático, usar resolução do source)
    void setOutputResolution(uint32_t width, uint32_t height)
    {
        m_outputWidth = width;
        m_outputHeight = height;
    }
    uint32_t getOutputWidth() const { return m_outputWidth; }
    uint32_t getOutputHeight() const { return m_outputHeight; }

    // Controles V4L2 (valores opcionais, -1 significa não configurar)
    void setV4L2Brightness(int32_t value) { m_v4l2Brightness = value; }
    void setV4L2Contrast(int32_t value) { m_v4l2Contrast = value; }
    void setV4L2Saturation(int32_t value) { m_v4l2Saturation = value; }
    void setV4L2Hue(int32_t value) { m_v4l2Hue = value; }
    void setV4L2Gain(int32_t value) { m_v4l2Gain = value; }
    void setV4L2Exposure(int32_t value) { m_v4l2Exposure = value; }
    void setV4L2Sharpness(int32_t value) { m_v4l2Sharpness = value; }
    void setV4L2Gamma(int32_t value) { m_v4l2Gamma = value; }
    void setV4L2WhiteBalance(int32_t value) { m_v4l2WhiteBalance = value; }

    // Streaming configuration
    void setStreamingEnabled(bool enabled) { m_streamingEnabled = enabled; }
    void setStreamingPort(uint16_t port) { m_streamingPort = port; }
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

    // Web Portal configuration
    void setWebPortalEnabled(bool enabled) { m_webPortalEnabled = enabled; }
    void setWebPortalHTTPSEnabled(bool enabled) { m_webPortalHTTPSEnabled = enabled; }
    void setWebPortalSSLCertPath(const std::string &path) { m_webPortalSSLCertPath = path; }
    void setWebPortalSSLKeyPath(const std::string &path) { m_webPortalSSLKeyPath = path; }

    // Recording configuration
    void setRecordingSettings(const struct RecordingSettings& settings);
    struct RecordingSettings getRecordingSettings() const;
    bool startRecording();
    void stopRecording();

private:
    // Build the metadata context for a recording session (#59): pulls
    // shader / source / nickname / version off the UI and capture
    // pipeline so RecordingManager can embed it as MP4/MKV metadata.
    void populateRecordingContext();
public:
    bool isRecording() const;
    uint64_t getRecordingDurationUs();
    uint64_t getRecordingFileSize();
    std::string getRecordingFilename();
    std::vector<struct RecordingMetadata> listRecordings();
    bool deleteRecording(const std::string& recordingId);
    bool renameRecording(const std::string& recordingId, const std::string& newName);
    std::string getRecordingPath(const std::string& recordingId);

    // API access methods
    ShaderEngine *getShaderEngine() { return m_shaderEngine.get(); }
    UIManager *getUIManager() { return m_ui.get(); }
    RecordingManager *getRecordingManager() { return m_recordingManager.get(); }
    IAudioCapture* getAudioCapture() const { return m_audioCapture.get(); }
    IVideoCapture* getVideoCapture() const { return m_capture.get(); }

    // Preset management
    void applyPreset(const std::string& presetName);
    void schedulePresetApplication(const std::string& presetName); // Thread-safe: schedules for main thread
    void createPresetFromCurrentState(const std::string& name, const std::string& description, bool captureThumbnail = false);
    
    // Shader path resolution (centralized)
    std::string resolveShaderPath(const std::string& shaderPath) const;

    // Phase 4 of #47: drains pending remote /meta snapshot onto the GL
    // thread. Called once per main-loop iteration; cheap no-op when
    // m_hasPendingRemoteMeta is false.
    void applyPendingRemoteMeta();

    // #49 Phase 2: reconciles the public-directory publish state with
    // the UI toggle. Called every frame; cheap when no transition.
    void syncDirectoryClient();
    fs::path getShaderBasePath() const;
    
    // Thread-safe resolution change scheduling
    void scheduleResolutionChange(uint32_t width, uint32_t height); // Thread-safe: schedules for main thread
    void applyResolutionChange(uint32_t width, uint32_t height); // Apply resolution change (called from main thread)

private:
    // Centralized cursor visibility management
    // Always syncs cursor visibility with UI visibility state
    void updateCursorVisibility();
    bool m_initialized = false;

    std::unique_ptr<IVideoCapture> m_capture;
#ifdef USE_SDL2
    std::unique_ptr<WindowManagerSDL> m_window;
#else
    std::unique_ptr<WindowManager> m_window;
#endif
    std::unique_ptr<OpenGLRenderer> m_renderer;
    std::unique_ptr<ShaderEngine> m_shaderEngine;
    std::unique_ptr<UIManager> m_ui;

    // #86 — system tray + hide-to-tray. m_tray is always non-null
    // (factory returns a no-op stub when the platform/DE has no tray
    // host). setupSystemTray() builds the icon + menu and wires the
    // window close callback; refreshTrayMenu() re-pushes labels/
    // enabled state each frame; m_trayActive reflects whether a real
    // tray host accepted the icon.
    std::unique_ptr<retrocapture::ISystemTray> m_tray;
    bool m_trayActive = false;
    uint32_t m_trayMenuSig = 0;      // change-detection for refreshTrayMenu
    bool m_trayMenuBuilt = false;
    void setupSystemTray();
    void refreshTrayMenu();
    std::unique_ptr<FrameProcessor> m_frameProcessor;
    std::unique_ptr<StreamManager> m_streamManager;
    std::unique_ptr<class DirectoryClient> m_directoryClient;       // #49 Phase 2
    std::unique_ptr<class CloudflaredManager> m_cloudflaredManager; // #49 Phase 2.5
    std::unique_ptr<class DirectoryBrowser> m_directoryBrowser;     // #49 Phase 4
    std::unique_ptr<class ChatClient>      m_chatClient;             // #84
    // #69 — Cache the URL each subsystem was started against so a
    // runtime edit in the UI reconfigures both immediately instead of
    // waiting for an app restart.
    std::string m_publishedDirectoryUrl;
    // #84 — Chat-service base URL override from --chat-url. Empty
    // means "no CLI override; let UIManager (loaded from config.json)
    // pick the value". UIManager owns the persistent default and the
    // runtime-editable field; Application syncs the UI value into
    // m_chatClient every frame via syncChatTransport().
    std::string m_chatBaseUrl;
    // #84 — Tracks the slug the chat client is currently bound to
    // (was streamId before the identity-bound rework). syncDirectory-
    // Client uses this to detect transitions on the host side; on
    // the viewer side, the remote /meta snapshot path uses the same
    // member to detect roomSlug changes.
    std::string m_chatBoundSlug;
    // #84 — Monotonic generation incremented on every chat
    // bind/unbind transition. The async stream-bind worker captures
    // it at spawn time and re-checks before calling connectBySlug
    // at the end; if the user clicked "Stop streaming" mid-flight
    // (or the slug otherwise changed), the worker's connect is
    // dropped so it doesn't undo the disconnect.
    std::atomic<uint64_t> m_chatBindEpoch{0};

    // #85 — Virtual-camera output sink. Linux uses v4l2loopback
    // (VirtualCameraOutput), Windows uses the shared-memory IPC
    // consumed by RetroCaptureVCam.dll (VirtualCameraOutputWin).
    // syncVirtualCamera() starts/stops it per m_ui->getVirtcamEnabled();
    // the main render loop calls pushFrame() on the RGBA scratch
    // right after PBOManager readback so the work piggybacks on
    // the existing readback path. Both sink classes expose the
    // same isRunning() / pushFrame(SourceFormat::RGB|RGBA) shape
    // so the call sites stay platform-agnostic.
#if defined(__linux__)
    using VirtcamSinkT = class VirtualCameraOutput;
#elif defined(_WIN32)
    using VirtcamSinkT = class VirtualCameraOutputWin;
#elif defined(__APPLE__)
    using VirtcamSinkT = class VirtualCameraOutputMac;
#endif
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
    std::unique_ptr<VirtcamSinkT> m_virtcam;
    void syncVirtualCamera();
#endif
    std::unique_ptr<IAudioCapture> m_audioCapture;
    std::unique_ptr<PBOManager> m_pboManager; // PBO para leitura assíncrona de pixels
    std::unique_ptr<RecordingManager> m_recordingManager;

    // Remote-source render pacing: when consuming a remote /raw stream the
    // client's main loop has no reason to iterate faster than the host's
    // source FPS. Without this cap an idle high-refresh display can
    // re-render the same decoded frame hundreds of times per source frame
    // — wasted GPU and CPU, plus the ImGui frame counter ends up showing
    // 500+ fps regardless of how slow the underlying source is.
    // sourceFps comes from /meta; m_lastFrameSwapUs tracks the timestamp
    // of the last completed main-loop iteration to compute the next sleep.
    uint32_t m_remoteSourceFps = 0;
    int64_t  m_lastFrameSwapUs = 0;

    // Phase 4 of #47: when source is Remote, this polls /meta and dispatches
    // shader/parameter deltas onto the main thread (see m_pendingRemote* below).
    std::unique_ptr<class RemoteMetaSync> m_remoteMetaSync;
    std::mutex                       m_pendingRemoteMutex;
    std::atomic<bool>                m_hasPendingRemoteMeta{false};
    std::string                      m_pendingRemotePreset;
    std::string                      m_pendingRemotePresetHash;
    std::string                      m_appliedRemotePresetHash;
    bool                             m_pendingRemotePipelineEnabled = true;
    std::vector<std::pair<std::string, float>> m_pendingRemoteParams;
    // Host's source resolution from /meta. The capture rescales /raw to
    // these dims so the local shader sees frames at the host's logical
    // source size, not the smaller transmission size.
    uint32_t                         m_pendingRemoteSourceWidth  = 0;
    uint32_t                         m_pendingRemoteSourceHeight = 0;
    // Host's Image-tab values from /meta. Only applied on the FIRST
    // snapshot after a connect — m_remoteImageSeeded gates the apply
    // so subsequent snapshots don't stomp whatever the local user
    // tweaked after the seed.
    bool                             m_remoteImageSeeded = false;
    float                            m_pendingRemoteImageBrightness     = 1.0f;
    float                            m_pendingRemoteImageContrast       = 1.0f;
    bool                             m_pendingRemoteImageMaintainAspect = true;
    uint32_t                         m_pendingRemoteImageOutputWidth    = 0;
    uint32_t                         m_pendingRemoteImageOutputHeight   = 0;
    bool                             m_hasPendingRemoteImageSeed        = false;

    // Buffers reutilizáveis no caminho de captura — evita alocar ~6MB/frame a 1080p.
    // pushFrame() copia os dados, então é seguro reutilizar.
    std::vector<uint8_t> m_captureFrameData;       // Saída RGB final (push-to-encoder).
    std::vector<uint8_t> m_captureRgbaScratch;     // Temp RGBA→RGB (path PBO com shader).
    std::vector<uint8_t> m_captureSyncPadded;      // Temp glReadPixels com row padding.
    // Per-pipeline shader override: secondary readback of the raw source
    // (pre-shader) so streaming/recording can opt out of the shader chain
    // independently of what the live preview shows.
    std::vector<uint8_t> m_captureSourceFrameData;
    std::vector<uint8_t> m_captureSourcePadded;

    // OTIMIZAÇÃO: Cache de SwsContext para resize (evitar criar/destruir a cada frame)

    // Configuração
    std::string m_shaderPath;
    std::string m_presetPath;
#if defined(_WIN32) || defined(__APPLE__)
    // Windows (DirectShow) and macOS (AVFoundation) both pick a
    // device by enumeration, not by filesystem path.
    std::string m_devicePath = "";
#else
    std::string m_devicePath = "/dev/video0"; // Linux: padrão V4L2
#endif
    uint32_t m_captureWidth = 1920;
    uint32_t m_captureHeight = 1080;
    uint32_t m_captureFps = 60;

    // Resolução LÓGICA: o que o usuário escolheu na UI. O driver V4L2 ajusta
    // m_captureWidth/Height pra mais próxima suportada. Quando lógica < capture,
    // fazemos um downscale antes do shader chain pra que efeitos de CRT/scanline
    // operem na escala que foram desenhados (típicamente 240p-480p), e não em
    // 1080p onde os scanlines viram sub-pixel e somem visualmente.
    uint32_t m_logicalCaptureWidth = 0;   // 0 = sem downscale (usa capture nativo)
    uint32_t m_logicalCaptureHeight = 0;
    GLuint m_shaderSourceFBO = 0;
    GLuint m_shaderSourceTexture = 0;
    uint32_t m_shaderSourceFBOWidth = 0;
    uint32_t m_shaderSourceFBOHeight = 0;
    uint32_t m_windowWidth = 1920;
    uint32_t m_windowHeight = 1080;
    
    // Resolução máxima de processamento (configurável pelo usuário)
    // 0 = sem limite (usar resolução original)
    uint32_t m_maxProcessingWidth = 0;
    uint32_t m_maxProcessingHeight = 0;
    
    // Resolução de saída configurável (aplicada após shader, antes de esticar para janela)
    // 0 = usar resolução do source (captura/shader output) - modo automático
    uint32_t m_outputWidth = 0;   // 0 = automático (usar source)
    uint32_t m_outputHeight = 0;  // 0 = automático (usar source)
    
    bool m_fullscreen = false;
    bool m_pendingFullscreenChange = false; // Flag para mudança de fullscreen pendente
    int m_monitorIndex = -1;                // -1 = usar monitor primário
    bool m_maintainAspect = false;
    float m_brightness = 1.0f;
    float m_contrast = 1.0f;
    
    // Texture filtering configurável (true = GL_LINEAR, false = GL_NEAREST)
    // GL_NEAREST é mais rápido e adequado para imagens pixel-perfect (retro)
    bool m_textureFilterLinear = false; // Padrão: GL_NEAREST para melhor performance

    // Controles V4L2 (-1 significa não configurar)
    int32_t m_v4l2Brightness = -1;
    int32_t m_v4l2Contrast = -1;
    int32_t m_v4l2Saturation = -1;
    int32_t m_v4l2Hue = -1;
    int32_t m_v4l2Gain = -1;
    int32_t m_v4l2Exposure = -1;
    int32_t m_v4l2Sharpness = -1;
    int32_t m_v4l2Gamma = -1;
    int32_t m_v4l2WhiteBalance = -1;

    // Streaming configuration
    bool m_streamingEnabled = false;
    uint16_t m_streamingPort = 8080;
    uint32_t m_streamingWidth = 640;                // Padrão: 640px (0 = usar resolução de captura)
    uint32_t m_streamingHeight = 480;               // Padrão: 480px (0 = usar resolução de captura)
    uint32_t m_streamingFps = 60;                   // 0 = usar FPS da captura
    uint32_t m_streamingBitrate = 8000;             // 0 = calcular automaticamente (vídeo)
    uint32_t m_streamingAudioBitrate = 256;         // 256 kbps (áudio)
    std::string m_streamingVideoCodec = "h264";     // Codec de vídeo: "h264", "h265", "vp8", "vp9"
    std::string m_streamingAudioCodec = "aac";      // Codec de áudio: "aac", "mp3", "opus"
    std::string m_streamingH264Preset = "veryfast"; // Preset H.264: ultrafast, superfast, veryfast, faster, fast, medium, slow, slower, veryslow
    std::string m_streamingH265Preset = "veryfast"; // Preset H.265: ultrafast, superfast, veryfast, faster, fast, medium, slow, slower, veryslow
    std::string m_streamingH265Profile = "main";    // Profile H.265: "main" (8-bit) ou "main10" (10-bit)
    std::string m_streamingH265Level = "auto";      // Level H.265: "auto", "1", "2", "2.1", "3", "3.1", "4", "4.1", "5", "5.1", "5.2", "6", "6.1", "6.2"
    int m_streamingVP8Speed = 12;                   // Speed VP8: 0-16 (0 = melhor qualidade, 16 = mais rápido, 12 = bom para streaming)
    int m_streamingVP9Speed = 6;                    // Speed VP9: 0-9 (0 = melhor qualidade, 9 = mais rápido, 6 = bom para streaming)
    int m_streamingHardwareEncoder = 0;             // 0 = Auto (MediaEncoder::HardwareEncoder enum value)
    std::string m_streamingNvencPreset = "p4";
    std::string m_streamingVaapiRcMode = "CBR";
    std::string m_streamingQsvPreset   = "veryfast";
    std::string m_streamingAmfQuality  = "speed";
    std::string m_remoteInterpolation  = "linear";
    bool        m_remoteWindowFocused  = true;  // tracks vsync toggle state in the Remote main-loop branch

    // Buffer configuration
    size_t m_streamingMaxVideoBufferSize = 10;     // Máximo de frames no buffer de vídeo (1-50)
    size_t m_streamingMaxAudioBufferSize = 20;     // Máximo de chunks no buffer de áudio (5-100)
    int64_t m_streamingMaxBufferTimeSeconds = 5;   // Tempo máximo de buffer em segundos (1-30)
    size_t m_streamingAVIOBufferSize = 256 * 1024; // 256KB para buffer AVIO do FFmpeg (64KB-1MB)

    // Referência ao streamer atual para atualização de configurações em tempo real
    HTTPTSStreamer *m_currentStreamer = nullptr;

    // Portal Web independente (servidor HTTP sem streaming)
    std::unique_ptr<HTTPTSStreamer> m_webPortalServer;
    bool m_webPortalActive = false;

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

    // Thread safety for resize operations
    mutable std::mutex m_resizeMutex;
    std::atomic<bool> m_isResizing{false};
    std::atomic<bool> m_isReconfiguring{false}; // Flag to prevent frame processing during reconfiguration
    
    // Thread-safe queue for preset application from API threads
    std::mutex m_presetQueueMutex;
    std::queue<std::string> m_pendingPresets;
    
    // Thread-safe queue for resolution changes from API threads
    struct ResolutionChange {
        uint32_t width;
        uint32_t height;
    };
    std::mutex m_resolutionQueueMutex;
    std::queue<ResolutionChange> m_pendingResolutionChanges;

    bool initCapture();
    bool reconfigureCapture(uint32_t width, uint32_t height, uint32_t fps);
    bool initWindow();
    bool initRenderer();
    bool initUI();
    bool initStreaming();
    bool initWebPortal();
    void stopWebPortal();
    bool initAudioCapture();
    void restoreAudioDeviceConnections();
    void handleKeyInput();
};
