#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Frame {
    uint8_t* data = nullptr;
    size_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0; // V4L2 pixel format
};

class VideoCapture {
public:
    VideoCapture();
    ~VideoCapture();
    
    bool open(const std::string& device);
    void close();
    
    bool isOpen() const { return m_fd >= 0; }
    
    bool setFormat(uint32_t width, uint32_t height, uint32_t pixelFormat = 0);
    bool setFramerate(uint32_t fps);
    bool startCapture();
    void stopCapture();
    
    bool captureFrame(Frame& frame);
    
    // Captura o frame mais recente, descartando frames antigos
    bool captureLatestFrame(Frame& frame);
    
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }
    uint32_t getPixelFormat() const { return m_pixelFormat; }
    
    // Lista formatos suportados
    std::vector<uint32_t> getSupportedFormats();
    
private:
    int m_fd = -1;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_pixelFormat = 0;
    
    // Buffers para memory mapping
    struct Buffer {
        void* start = nullptr;
        size_t length = 0;
    };
    
    std::vector<Buffer> m_buffers;
    bool m_streaming = false;
    
    bool initMemoryMapping();
    void cleanupBuffers();
    bool convertYUYVtoRGB(const Frame& input, std::vector<uint8_t>& output);
};

