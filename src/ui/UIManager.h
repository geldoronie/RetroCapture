#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <cstring>
#include "../renderer/glad_loader.h"

struct GLFWwindow;

class VideoCapture;
class ShaderEngine;

class UIManager
{
public:
    UIManager();
    ~UIManager();

    bool init(GLFWwindow *window);
    void shutdown();

    void beginFrame();
    void endFrame();

    void render();

    // Callbacks para interação
    void setShaderList(const std::vector<std::string> &shaders) { m_shaderList = shaders; }
    void setCurrentShader(const std::string &shader) { m_currentShader = shader; }
    void setOnShaderChanged(std::function<void(const std::string &)> callback) { m_onShaderChanged = callback; }

    // Parâmetros de shader
    void setShaderEngine(ShaderEngine *engine) { m_shaderEngine = engine; }
    void setOnSavePreset(std::function<void(const std::string &, bool)> callback) { m_onSavePreset = callback; }

    void setBrightness(float brightness) { m_brightness = brightness; }
    void setContrast(float contrast) { m_contrast = contrast; }
    void setOnBrightnessChanged(std::function<void(float)> callback) { m_onBrightnessChanged = callback; }
    void setOnContrastChanged(std::function<void(float)> callback) { m_onContrastChanged = callback; }

    void setMaintainAspect(bool maintain) { m_maintainAspect = maintain; }
    void setOnMaintainAspectChanged(std::function<void(bool)> callback) { m_onMaintainAspectChanged = callback; }

    void setFullscreen(bool fullscreen) { m_fullscreen = fullscreen; }
    void setOnFullscreenChanged(std::function<void(bool)> callback) { m_onFullscreenChanged = callback; }
    void setMonitorIndex(int index) { m_monitorIndex = index; }
    void setOnMonitorIndexChanged(std::function<void(int)> callback) { m_onMonitorIndexChanged = callback; }

    // Controles V4L2
    void setV4L2Controls(VideoCapture *capture);
    void setOnV4L2ControlChanged(std::function<void(const std::string &, int32_t)> callback)
    {
        m_onV4L2ControlChanged = callback;
    }
    void setOnDeviceChanged(std::function<void(const std::string &)> callback) { m_onDeviceChanged = callback; }
    void setCurrentDevice(const std::string &device) { m_currentDevice = device; }
    void refreshV4L2Devices();

    // Informações da captura
    void setCaptureInfo(uint32_t width, uint32_t height, uint32_t fps, const std::string &device);
    void setOnResolutionChanged(std::function<void(uint32_t, uint32_t)> callback) { m_onResolutionChanged = callback; }
    void setOnFramerateChanged(std::function<void(uint32_t)> callback) { m_onFramerateChanged = callback; }

    // Visibilidade da UI
    bool isVisible() const { return m_uiVisible; }
    void setVisible(bool visible) { m_uiVisible = visible; }
    void toggle() { m_uiVisible = !m_uiVisible; }

    // Streaming info setters (public)
    void setStreamingActive(bool active) { m_streamingActive = active; }
    void setStreamUrl(const std::string &url) { m_streamUrl = url; }
    void setStreamClientCount(uint32_t count) { m_streamClientCount = count; }
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

    // Buffer settings
    void setStreamingMaxVideoBufferSize(size_t size) { m_streamingMaxVideoBufferSize = size; }
    void setStreamingMaxAudioBufferSize(size_t size) { m_streamingMaxAudioBufferSize = size; }
    void setStreamingMaxBufferTimeSeconds(int64_t seconds) { m_streamingMaxBufferTimeSeconds = seconds; }
    void setStreamingMaxHLSBufferSize(size_t size) { m_streamingMaxHLSBufferSize = size; }
    void setStreamingAVIOBufferSize(size_t size) { m_streamingAVIOBufferSize = size; }

    size_t getStreamingMaxVideoBufferSize() const { return m_streamingMaxVideoBufferSize; }
    size_t getStreamingMaxAudioBufferSize() const { return m_streamingMaxAudioBufferSize; }
    int64_t getStreamingMaxBufferTimeSeconds() const { return m_streamingMaxBufferTimeSeconds; }
    size_t getStreamingMaxHLSBufferSize() const { return m_streamingMaxHLSBufferSize; }
    size_t getStreamingAVIOBufferSize() const { return m_streamingAVIOBufferSize; }

    // HLS Performance settings
    void setHLSLowLatencyMode(bool enabled) { m_hlsLowLatencyMode = enabled; }
    void setHLSBackBufferLength(float seconds) { m_hlsBackBufferLength = seconds; }
    void setHLSMaxBufferLength(float seconds) { m_hlsMaxBufferLength = seconds; }
    void setHLSMaxMaxBufferLength(float seconds) { m_hlsMaxMaxBufferLength = seconds; }
    void setHLSEnableWorker(bool enabled) { m_hlsEnableWorker = enabled; }

    bool getHLSLowLatencyMode() const { return m_hlsLowLatencyMode; }
    float getHLSBackBufferLength() const { return m_hlsBackBufferLength; }
    float getHLSMaxBufferLength() const { return m_hlsMaxBufferLength; }
    float getHLSMaxMaxBufferLength() const { return m_hlsMaxMaxBufferLength; }
    bool getHLSEnableWorker() const { return m_hlsEnableWorker; }

    void setOnHLSLowLatencyModeChanged(std::function<void(bool)> callback) { m_onHLSLowLatencyModeChanged = callback; }
    void setOnHLSBackBufferLengthChanged(std::function<void(float)> callback) { m_onHLSBackBufferLengthChanged = callback; }
    void setOnHLSMaxBufferLengthChanged(std::function<void(float)> callback) { m_onHLSMaxBufferLengthChanged = callback; }
    void setOnHLSMaxMaxBufferLengthChanged(std::function<void(float)> callback) { m_onHLSMaxMaxBufferLengthChanged = callback; }
    void setOnHLSEnableWorkerChanged(std::function<void(bool)> callback) { m_onHLSEnableWorkerChanged = callback; }

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

    // Image settings getters
    float getBrightness() const { return m_brightness; }
    float getContrast() const { return m_contrast; }
    bool getMaintainAspect() const { return m_maintainAspect; }
    bool getFullscreen() const { return m_fullscreen; }
    int getMonitorIndex() const { return m_monitorIndex; }
    std::string getCurrentShader() const { return m_currentShader; }

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

private:
    bool m_initialized = false;
    bool m_uiVisible = true;
    bool m_configWindowVisible = true;    // Janela de configuração visível por padrão
    bool m_configWindowJustOpened = true; // Flag para aplicar posição/tamanho inicial apenas quando aberta
    GLFWwindow *m_window = nullptr;

    // Shader selection
    std::vector<std::string> m_shaderList;
    std::string m_currentShader;
    int m_selectedShaderIndex = 0;
    std::function<void(const std::string &)> m_onShaderChanged;
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

    // V4L2 Controls
    VideoCapture *m_capture = nullptr;
    struct V4L2Control
    {
        std::string name;
        int32_t value;
        int32_t min;
        int32_t max;
        int32_t step;
        bool available;
    };
    std::vector<V4L2Control> m_v4l2Controls;
    std::function<void(const std::string &, int32_t)> m_onV4L2ControlChanged;

    // Device selection
    std::vector<std::string> m_v4l2Devices;
    std::string m_currentDevice;
    std::function<void(const std::string &)> m_onDeviceChanged;

    // Capture info
    uint32_t m_captureWidth = 0;
    uint32_t m_captureHeight = 0;
    uint32_t m_captureFps = 0;
    std::string m_captureDevice;
    std::function<void(uint32_t, uint32_t)> m_onResolutionChanged;
    std::function<void(uint32_t)> m_onFramerateChanged;

    // UI helpers
    void renderShaderPanel();
    void renderImageControls();
    void renderV4L2Controls();
    void renderInfoPanel();
    void renderStreamingPanel();
    void renderWebPortalPanel();

    // Scanning methods
    void scanShaders(const std::string &basePath);
    void scanV4L2Devices();

    // Configuration persistence
    void loadConfig();
    void saveConfig();
    std::string getConfigPath() const;

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
    bool m_streamingActive = false;
    std::string m_streamUrl = "";
    uint32_t m_streamClientCount = 0;

    // Buffer configuration (para economizar memória, especialmente em ARM)
    size_t m_streamingMaxVideoBufferSize = 10;            // Máximo de frames no buffer de vídeo
    size_t m_streamingMaxAudioBufferSize = 20;            // Máximo de chunks no buffer de áudio
    int64_t m_streamingMaxBufferTimeSeconds = 5;          // Tempo máximo de buffer em segundos
    size_t m_streamingMaxHLSBufferSize = 2 * 1024 * 1024; // 2MB máximo para buffer HLS
    size_t m_streamingAVIOBufferSize = 256 * 1024;        // 256KB para buffer AVIO do FFmpeg

    // HLS Performance parameters
    bool m_hlsLowLatencyMode = true;       // Modo de baixa latência
    float m_hlsBackBufferLength = 30.0f;   // Tamanho do buffer de retaguarda em segundos (reduzido para evitar bufferFullError)
    float m_hlsMaxBufferLength = 10.0f;    // Tamanho máximo do buffer em segundos (reduzido para baixa latência)
    float m_hlsMaxMaxBufferLength = 30.0f; // Tamanho máximo absoluto do buffer em segundos (reduzido)
    bool m_hlsEnableWorker = true;         // Usar Web Worker para processamento

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
    std::function<void(bool)> m_onHLSLowLatencyModeChanged;
    std::function<void(float)> m_onHLSBackBufferLengthChanged;
    std::function<void(float)> m_onHLSMaxBufferLengthChanged;
    std::function<void(float)> m_onHLSMaxMaxBufferLengthChanged;
    std::function<void(bool)> m_onHLSEnableWorkerChanged;

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
};
