#pragma once

#include <string>
#include <cstdint>
#include "../renderer/glad_loader.h"

class VideoCapture;
class WindowManager;
class OpenGLRenderer;
class ShaderEngine;

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
    void setBrightness(float brightness) { m_brightness = brightness; }
    void setContrast(float contrast) { m_contrast = contrast; }
    
private:
    bool m_initialized = false;
    
    VideoCapture* m_capture = nullptr;
    WindowManager* m_window = nullptr;
    OpenGLRenderer* m_renderer = nullptr;
    ShaderEngine* m_shaderEngine = nullptr;
    
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
    float m_brightness = 1.0f;
    float m_contrast = 1.0f;
    
    bool initCapture();
    bool initWindow();
    bool initRenderer();
    bool processFrame(); // Retorna true se um novo frame foi processado
    void convertYUYVtoRGB(const uint8_t* yuyv, uint8_t* rgb, uint32_t width, uint32_t height);
};

