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

class VideoCapture;
class WindowManager;
class OpenGLRenderer;
class ShaderEngine;
class UIManager;
class FrameProcessor;
class StreamManager;
class AudioCapture;

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
    void setStreamingQuality(int quality) { m_streamingQuality = quality; }
    void setStreamingVideoCodec(const std::string &codec) { m_streamingVideoCodec = codec; }
    void setStreamingAudioCodec(const std::string &codec) { m_streamingAudioCodec = codec; }
    void setStreamingAudioBufferSize(uint32_t frames) { m_streamingAudioBufferSize = frames; }

private:
    bool m_initialized = false;

    std::unique_ptr<VideoCapture> m_capture;
    std::unique_ptr<WindowManager> m_window;
    std::unique_ptr<OpenGLRenderer> m_renderer;
    std::unique_ptr<ShaderEngine> m_shaderEngine;
    std::unique_ptr<UIManager> m_ui;
    std::unique_ptr<FrameProcessor> m_frameProcessor;
    std::unique_ptr<StreamManager> m_streamManager;
    std::unique_ptr<AudioCapture> m_audioCapture;

    // Streaming thread
    std::thread m_streamingThread;
    std::atomic<bool> m_streamingThreadRunning{false};
    void streamingThreadFunc();

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
    uint32_t m_streamingWidth = 640;            // Padrão: 640px (0 = usar resolução de captura)
    uint32_t m_streamingHeight = 480;           // Padrão: 480px (0 = usar resolução de captura)
    uint32_t m_streamingFps = 60;               // 0 = usar FPS da captura
    uint32_t m_streamingBitrate = 8000;         // 0 = calcular automaticamente (vídeo)
    uint32_t m_streamingAudioBitrate = 256;     // 256 kbps (áudio)
    int m_streamingQuality = 85;                // Qualidade JPEG (1-100) - não usado mais, mantido para compatibilidade
    std::string m_streamingVideoCodec = "h264"; // Codec de vídeo: "h264", "h265", "vp8", "vp9"
    std::string m_streamingAudioCodec = "aac";  // Codec de áudio: "aac", "mp3", "opus"
    uint32_t m_streamingAudioBufferSize = 50;   // Tamanho do buffer de áudio em frames (padrão: 50 = ~1 segundo a 48kHz)

    // Thread safety for resize operations
    mutable std::mutex m_resizeMutex;
    std::atomic<bool> m_isResizing{false};
    
    // Fila de frames para streaming thread (captura de vídeo)
    // Usar fila em vez de buffer único para evitar perda de frames
    struct SharedFrameData {
        std::vector<uint8_t> frameData;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    mutable std::mutex m_frameDataMutex;
    std::deque<SharedFrameData> m_frameQueue; // Fila de frames para processar
    static constexpr size_t MAX_FRAME_QUEUE_SIZE = 60; // Aumentar para 60 frames (1 segundo a 60 FPS) para evitar perda

    bool initCapture();
    bool reconfigureCapture(uint32_t width, uint32_t height, uint32_t fps);
    bool initWindow();
    bool initRenderer();
    bool initUI();
    bool initStreaming();
    bool initAudioCapture();
    void handleKeyInput();
};
