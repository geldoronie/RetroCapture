#pragma once

#include "IStreamer.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <memory>
#include <cstdint>

/**
 * HTTP MJPEG streamer implementation.
 * 
 * Serves Motion JPEG stream over HTTP using multipart/x-mixed-replace.
 * Simple implementation using raw sockets (no external HTTP library required).
 */
class HTTPMJPEGStreamer : public IStreamer {
public:
    HTTPMJPEGStreamer();
    ~HTTPMJPEGStreamer() override;
    
    std::string getType() const override { return "HTTP MJPEG"; }
    bool initialize(uint16_t port, uint32_t width, uint32_t height, uint32_t fps) override;
    bool start() override;
    void stop() override;
    bool isActive() const override;
    bool pushFrame(const uint8_t* data, uint32_t width, uint32_t height) override;
    std::string getStreamUrl() const override;
    uint32_t getClientCount() const override;
    void cleanup() override;
    
    // Additional configuration methods
    void setQuality(int quality) { m_jpegQuality = quality; }
    void setBitrate(uint32_t bitrate) { m_bitrate = bitrate; }
    
private:
    void serverThread();
    void handleClient(int clientFd);
    bool encodeJPEG(const uint8_t* rgbData, uint32_t width, uint32_t height, std::vector<uint8_t>& jpegData);
    
    uint16_t m_port = 8080;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_fps = 30;
    int m_jpegQuality = 85; // JPEG quality (1-100, higher = better quality)
    uint32_t m_bitrate = 0; // Bitrate em bps (0 = calcular automaticamente)
    
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_running{false};
    std::thread m_serverThread;
    
    int m_serverSocket = -1;
    std::atomic<uint32_t> m_clientCount{0};
    
    // Latest frame buffer
    std::mutex m_frameMutex;
    std::vector<uint8_t> m_latestFrame;
    uint32_t m_frameWidth = 0;
    uint32_t m_frameHeight = 0;
    bool m_hasFrame = false;
    
    // Frame counter for PTS
    int64_t m_frameCount = 0;
};

