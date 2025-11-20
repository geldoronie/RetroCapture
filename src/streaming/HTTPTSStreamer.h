#pragma once

#include "IStreamer.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <memory>
#include <cstdint>
#include <queue>
#include <utility>

/**
 * HTTP MPEG-TS streamer implementation.
 *
 * Serves audio+video stream over HTTP using MPEG-TS container.
 * Uses FFmpeg to mux H.264 video and AAC audio into MPEG-TS.
 *
 * ABORDAGEM OBS: Buffers independentes, master clock de áudio, sincronização via timestamps
 */
class HTTPTSStreamer : public IStreamer
{
public:
    HTTPTSStreamer();
    ~HTTPTSStreamer() override;

    std::string getType() const override { return "HTTP MPEG-TS"; }
    bool initialize(uint16_t port, uint32_t width, uint32_t height, uint32_t fps) override;
    bool start() override;
    void stop() override;
    bool isActive() const override;
    bool pushFrame(const uint8_t *data, uint32_t width, uint32_t height) override;
    bool pushAudio(const int16_t *samples, size_t sampleCount) override;
    std::string getStreamUrl() const override;
    uint32_t getClientCount() const override;
    void cleanup() override;

    // Additional configuration methods
    void setVideoBitrate(uint32_t bitrate) { m_videoBitrate = bitrate; }
    void setAudioBitrate(uint32_t bitrate) { m_audioBitrate = bitrate; }
    void setAudioFormat(uint32_t sampleRate, uint32_t channels);
    void setVideoCodec(const std::string &codecName);
    void setAudioCodec(const std::string &codecName);

    // Public for static callback
    int writeToClients(const uint8_t *buf, int buf_size);

private:
    void serverThread();
    void handleClient(int clientFd);
    void encodingThread(); // Thread única para encoding
    bool initializeFFmpeg();
    bool initializeVideoCodec();
    bool initializeAudioCodec();
    bool initializeMuxers();
    void cleanupFFmpeg();
    void flushCodecs();
    bool encodeVideoFrame(const uint8_t *rgbData, uint32_t width, uint32_t height);
    bool encodeAudioFrame(const int16_t *samples, size_t sampleCount);
    bool muxPacket(void *packet);

    uint16_t m_port = 8080;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_fps = 60;                // Taxa de frames configurada via initialize() - NUNCA hardcoded nos cálculos
    uint32_t m_videoBitrate = 2000000;  // 2 Mbps
    uint32_t m_audioBitrate = 128000;   // 128 kbps
    uint32_t m_audioSampleRate = 48000; // 48 kHz
    uint32_t m_audioChannelsCount = 2;  // 2 channels

    // Codec selection
    std::string m_videoCodecName = "h264";
    std::string m_audioCodecName = "aac";

    // Codec contexts
    AVCodecContext *m_videoCodecContext = nullptr;
    AVCodecContext *m_audioCodecContext = nullptr;
    AVFormatContext *m_muxerContext = nullptr;

    std::atomic<bool> m_active{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cleanedUp{false};

    // Frame Queue
    std::mutex m_frameQueueMutex;
    std::queue<std::pair<std::shared_ptr<std::vector<uint8_t>>, std::pair<uint32_t, uint32_t>>> m_frameQueue;

    // Audio Queue
    std::mutex m_audioQueueMutex;
    std::queue<std::pair<std::shared_ptr<std::vector<int16_t>>, std::pair<size_t, size_t>>> m_audioQueue;

    // Threads
    std::thread m_serverThread;
    std::thread m_encodingThread; // Thread única para encoding

    int m_serverSocket = -1;
    std::atomic<uint32_t> m_clientCount{0};

    // Output buffer for clients
    std::vector<int> m_clientSockets;
};
