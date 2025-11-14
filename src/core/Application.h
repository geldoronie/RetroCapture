#pragma once

#include <string>
#include <cstdint>
#include <cinttypes>
#include <memory>
#include <mutex>
#include <atomic>
#include "../renderer/glad_loader.h"

class VideoCapture;
class WindowManager;
class OpenGLRenderer;
class ShaderEngine;
class UIManager;
class FrameProcessor;
class StreamManager;

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
    
    // Streaming configuration
    void setStreamingEnabled(bool enabled) { m_streamingEnabled = enabled; }
    void setStreamingPort(uint16_t port) { m_streamingPort = port; }
    void setStreamingWidth(uint32_t width) { m_streamingWidth = width; }
    void setStreamingHeight(uint32_t height) { m_streamingHeight = height; }
    void setStreamingFps(uint32_t fps) { m_streamingFps = fps; }
    void setStreamingBitrate(uint32_t bitrate) { m_streamingBitrate = bitrate; }
    void setStreamingQuality(int quality) { m_streamingQuality = quality; }
    
private:
    bool m_initialized = false;
    
    std::unique_ptr<VideoCapture> m_capture;
    std::unique_ptr<WindowManager> m_window;
    std::unique_ptr<OpenGLRenderer> m_renderer;
    std::unique_ptr<ShaderEngine> m_shaderEngine;
    std::unique_ptr<UIManager> m_ui;
    std::unique_ptr<FrameProcessor> m_frameProcessor;
    std::unique_ptr<StreamManager> m_streamManager;
    
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
    
    // Streaming configuration
    bool m_streamingEnabled = false;
    uint16_t m_streamingPort = 8080;
    uint32_t m_streamingWidth = 0;  // 0 = usar largura da janela
    uint32_t m_streamingHeight = 0; // 0 = usar altura da janela
    uint32_t m_streamingFps = 0;    // 0 = usar FPS da captura
    uint32_t m_streamingBitrate = 0; // 0 = calcular automaticamente
    int m_streamingQuality = 85;     // Qualidade JPEG (1-100)
    
    // Thread safety for resize operations
    mutable std::mutex m_resizeMutex;
    std::atomic<bool> m_isResizing{false};
    
    bool initCapture();
    bool reconfigureCapture(uint32_t width, uint32_t height, uint32_t fps);
    bool initWindow();
    bool initRenderer();
    bool initUI();
    bool initStreaming();
    void handleKeyInput();
};

