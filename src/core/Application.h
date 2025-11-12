#pragma once

#include <string>
#include <cstdint>
#include <cinttypes>
#include "../renderer/glad_loader.h"

class VideoCapture;
class WindowManager;
class OpenGLRenderer;
class ShaderEngine;
class UIManager;

class Application {
public:
    Application();
    ~Application();
    
    bool init();
    void run();
    void shutdown();
    
    // Configuração antes de init()
    void setShaderPath(const std::string& path) { m_shaderPath = path; }
    void setPresetPath(const std::string& path) { m_presetPath = path; }
    void setDevicePath(const std::string& path) { m_devicePath = path; }
    void setResolution(uint32_t width, uint32_t height) { m_captureWidth = width; m_captureHeight = height; }
    void setFramerate(uint32_t fps) { m_captureFps = fps; }
    void setWindowSize(uint32_t width, uint32_t height) { m_windowWidth = width; m_windowHeight = height; }
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
    
private:
    bool m_initialized = false;
    
    VideoCapture* m_capture = nullptr;
    WindowManager* m_window = nullptr;
    OpenGLRenderer* m_renderer = nullptr;
    ShaderEngine* m_shaderEngine = nullptr;
    UIManager* m_ui = nullptr;
    
    GLuint m_texture = 0;
    uint32_t m_textureWidth = 0;
    uint32_t m_textureHeight = 0;
    bool m_hasValidFrame = false;
    
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
    int m_monitorIndex = -1; // -1 = usar monitor primário
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
    
    bool initCapture();
    bool initWindow();
    bool initRenderer();
    bool initUI();
    bool processFrame(); // Retorna true se um novo frame foi processado
    void convertYUYVtoRGB(const uint8_t* yuyv, uint8_t* rgb, uint32_t width, uint32_t height);
    void handleKeyInput();
};

