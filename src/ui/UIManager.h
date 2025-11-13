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

    std::vector<std::string> m_scannedShaders;
    std::string m_shaderBasePath = "shaders/shaders_glsl";
    
    // Save preset
    std::function<void(const std::string&, bool)> m_onSavePreset; // path, overwrite
    char m_savePresetPath[512] = "";
    bool m_showSaveDialog = false;
};
