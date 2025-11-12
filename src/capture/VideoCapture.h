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
    
    // Controles V4L2
    bool setControl(uint32_t controlId, int32_t value);
    bool getControl(uint32_t controlId, int32_t& value);
    bool getControl(uint32_t controlId, int32_t& value, int32_t& min, int32_t& max, int32_t& step);
    bool setBrightness(int32_t value);      // -100 a 100 (padrão: 0)
    bool setContrast(int32_t value);        // -100 a 100 (padrão: 0)
    bool setSaturation(int32_t value);      // -100 a 100 (padrão: 0)
    bool setHue(int32_t value);            // -100 a 100 (padrão: 0)
    bool setGain(int32_t value);           // 0 a 100 (padrão: 0)
    bool setExposure(int32_t value);        // -13 a 1 (padrão: -6)
    bool setSharpness(int32_t value);       // 0 a 6 (padrão: 3)
    bool setGamma(int32_t value);          // 100 a 300 (padrão: 100)
    bool setWhiteBalanceTemperature(int32_t value); // 2800 a 6500 (padrão: 4600)
    
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

