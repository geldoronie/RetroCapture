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
#include "../renderer/glad_loader.h"

class IVideoCapture;
class IAudioCapture;
class WindowManager;
class OpenGLRenderer;
class ShaderEngine;
class UIManager;
class FrameProcessor;
class StreamManager;
class HTTPTSStreamer;

// Forward declaration for API
struct ShaderParameter;

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
    void setBrightness(float brightness) { m_brightness = brightness; }
    void setContrast(float contrast) { m_contrast = contrast; }

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

    // API access methods
    ShaderEngine *getShaderEngine() { return m_shaderEngine.get(); }
    UIManager *getUIManager() { return m_ui.get(); }

private:
    bool m_initialized = false;

    std::unique_ptr<IVideoCapture> m_capture;
    std::unique_ptr<WindowManager> m_window;
    std::unique_ptr<OpenGLRenderer> m_renderer;
    std::unique_ptr<ShaderEngine> m_shaderEngine;
    std::unique_ptr<UIManager> m_ui;
    std::unique_ptr<FrameProcessor> m_frameProcessor;
    std::unique_ptr<StreamManager> m_streamManager;
    std::unique_ptr<IAudioCapture> m_audioCapture;

    // OTIMIZAÇÃO: Cache de SwsContext para resize (evitar criar/destruir a cada frame)

    // Streaming thread
    // OPÇÃO A: Thread de streaming removida - processamento movido para thread principal

    // Configuração
    std::string m_shaderPath;
    std::string m_presetPath;
    std::string m_devicePath = "/dev/video0";
    uint32_t m_captureWidth = 1920;
    uint32_t m_captureHeight = 1080;
    uint32_t m_captureFps = 60;
    uint32_t m_windowWidth = 1920;
    uint32_t m_windowHeight = 1080;
    bool m_fullscreen = false;
    bool m_pendingFullscreenChange = false; // Flag para mudança de fullscreen pendente
    int m_monitorIndex = -1;                // -1 = usar monitor primário
    bool m_maintainAspect = false;
    float m_brightness = 1.0f;
    float m_contrast = 1.0f;

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
    std::string m_webPortalSubtitle = "Streaming de vídeo em tempo real"; // Subtítulo
    std::string m_webPortalImagePath = "logo.png";                        // Caminho da imagem para o título (padrão: logo.png)
    std::string m_webPortalBackgroundImagePath;                           // Caminho da imagem de fundo (opcional)

    // Textos editáveis dos cards
    std::string m_webPortalTextStreamInfo = "Informações do Stream";
    std::string m_webPortalTextQuickActions = "Ações Rápidas";
    std::string m_webPortalTextCompatibility = "Compatibilidade";
    std::string m_webPortalTextStatus = "Status";
    std::string m_webPortalTextCodec = "Codec";
    std::string m_webPortalTextResolution = "Resolução";
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

    // Fila de frames para streaming thread (captura de vídeo)
    // OPÇÃO A: Fila removida - frames processados diretamente na thread principal

    bool initCapture();
    bool reconfigureCapture(uint32_t width, uint32_t height, uint32_t fps);
    bool initWindow();
    bool initRenderer();
    bool initUI();
    bool initStreaming();
    bool initWebPortal();
    void stopWebPortal();
    bool initAudioCapture();
    void handleKeyInput();
};
