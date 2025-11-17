#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
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
    void setShaderEngine(ShaderEngine* engine) { m_shaderEngine = engine; }
    void setOnSavePreset(std::function<void(const std::string&, bool)> callback) { m_onSavePreset = callback; }

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
    void setCurrentDevice(const std::string& device) { m_currentDevice = device; }
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
    void setStreamUrl(const std::string& url) { m_streamUrl = url; }
    void setStreamClientCount(uint32_t count) { m_streamClientCount = count; }
    void setStreamingPort(uint16_t port) { m_streamingPort = port; }
    void setStreamingWidth(uint32_t width) { m_streamingWidth = width; }
    void setStreamingHeight(uint32_t height) { m_streamingHeight = height; }
    void setStreamingFps(uint32_t fps) { m_streamingFps = fps; }
    void setStreamingBitrate(uint32_t bitrate) { m_streamingBitrate = bitrate; }
    void setStreamingAudioBitrate(uint32_t bitrate) { m_streamingAudioBitrate = bitrate; }
    void setStreamingQuality(int quality) { m_streamingQuality = quality; }
    void setStreamingVideoCodec(const std::string& codec) { m_streamingVideoCodec = codec; }
    void setStreamingAudioCodec(const std::string& codec) { m_streamingAudioCodec = codec; }
    
    // Streaming callbacks
    void setOnStreamingStartStop(std::function<void(bool)> callback) { m_onStreamingStartStop = callback; }
    void setOnStreamingPortChanged(std::function<void(uint16_t)> callback) { m_onStreamingPortChanged = callback; }
    void setOnStreamingWidthChanged(std::function<void(uint32_t)> callback) { m_onStreamingWidthChanged = callback; }
    void setOnStreamingHeightChanged(std::function<void(uint32_t)> callback) { m_onStreamingHeightChanged = callback; }
    void setOnStreamingFpsChanged(std::function<void(uint32_t)> callback) { m_onStreamingFpsChanged = callback; }
    void setOnStreamingBitrateChanged(std::function<void(uint32_t)> callback) { m_onStreamingBitrateChanged = callback; }
    void setOnStreamingAudioBitrateChanged(std::function<void(uint32_t)> callback) { m_onStreamingAudioBitrateChanged = callback; }
    void setOnStreamingQualityChanged(std::function<void(int)> callback) { m_onStreamingQualityChanged = callback; }
    void setOnStreamingVideoCodecChanged(std::function<void(const std::string&)> callback) { m_onStreamingVideoCodecChanged = callback; }
    void setOnStreamingAudioCodecChanged(std::function<void(const std::string&)> callback) { m_onStreamingAudioCodecChanged = callback; }

private:
    bool m_initialized = false;
    bool m_uiVisible = true;
    GLFWwindow *m_window = nullptr;

    // Shader selection
    std::vector<std::string> m_shaderList;
    std::string m_currentShader;
    int m_selectedShaderIndex = 0;
    std::function<void(const std::string &)> m_onShaderChanged;
    ShaderEngine* m_shaderEngine = nullptr;

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
    
    // Scanning methods
    void scanShaders(const std::string &basePath);
    void scanV4L2Devices();

    std::vector<std::string> m_scannedShaders;
    std::string m_shaderBasePath = "shaders/shaders_glsl";
    
    // Save preset
    std::function<void(const std::string&, bool)> m_onSavePreset; // path, overwrite
    char m_savePresetPath[512] = "";
    bool m_showSaveDialog = false;
    
    // Streaming controls
    bool m_streamingEnabled = false;
    uint16_t m_streamingPort = 8080;
    uint32_t m_streamingWidth = 0;
    uint32_t m_streamingHeight = 0;
    uint32_t m_streamingFps = 0;
    uint32_t m_streamingBitrate = 0;
    uint32_t m_streamingAudioBitrate = 128;
    int m_streamingQuality = 85;
    std::string m_streamingVideoCodec = "h264";
    std::string m_streamingAudioCodec = "aac";
    bool m_streamingActive = false;
    std::string m_streamUrl = "";
    uint32_t m_streamClientCount = 0;
    
    std::function<void(bool)> m_onStreamingStartStop;
    std::function<void(uint16_t)> m_onStreamingPortChanged;
    std::function<void(uint32_t)> m_onStreamingWidthChanged;
    std::function<void(uint32_t)> m_onStreamingHeightChanged;
    std::function<void(uint32_t)> m_onStreamingFpsChanged;
    std::function<void(uint32_t)> m_onStreamingBitrateChanged;
    std::function<void(uint32_t)> m_onStreamingAudioBitrateChanged;
    std::function<void(int)> m_onStreamingQualityChanged;
    std::function<void(const std::string&)> m_onStreamingVideoCodecChanged;
    std::function<void(const std::string&)> m_onStreamingAudioCodecChanged;
};
