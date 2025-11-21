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
#include <string>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <thread>
#include <chrono>
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

    // Funções de conversão
    bool convertRGBToYUV(const uint8_t *rgbData, uint32_t width, uint32_t height, void *videoFrame);
    bool convertInt16ToFloatPlanar(const int16_t *samples, size_t sampleCount, void *audioFrame, size_t outputSamples);

    uint16_t m_port = 8080;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_fps = 60;                // Taxa de frames configurada via initialize() - NUNCA hardcoded nos cálculos
    uint32_t m_videoBitrate = 2000000;  // 2 Mbps
    uint32_t m_audioBitrate = 128000;   // 128 kbps
    uint32_t m_audioSampleRate = 44100; // 44.1 kHz
    uint32_t m_audioChannelsCount = 2;  // 2 channels

    // Codec selection
    std::string m_videoCodecName = "h264";
    std::string m_audioCodecName = "aac";

    // Codec contexts (usando void* para evitar incluir headers FFmpeg no .h)
    void *m_videoCodecContext = nullptr; // AVCodecContext*
    void *m_audioCodecContext = nullptr; // AVCodecContext*
    void *m_muxerContext = nullptr;      // AVFormatContext*
    void *m_videoStream = nullptr;       // AVStream* (stream de vídeo no muxer)
    void *m_audioStream = nullptr;       // AVStream* (stream de áudio no muxer)

    // FFmpeg conversion contexts
    void *m_swsContext = nullptr; // SwsContext* (RGB to YUV)
    void *m_swrContext = nullptr; // SwrContext* (int16 to float planar)
    void *m_videoFrame = nullptr; // AVFrame* (para encoding de vídeo)
    void *m_audioFrame = nullptr; // AVFrame* (para encoding de áudio)

    std::atomic<bool> m_active{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cleanedUp{false};

    // Frame Queue (raw frames - antes do encoding)
    static constexpr size_t MAX_VIDEO_QUEUE_SIZE = 30;  // Limite de frames brutos na fila
    static constexpr size_t MAX_AUDIO_QUEUE_SIZE = 100; // Limite de chunks de áudio bruto na fila
    std::mutex m_frameQueueMutex;
    std::queue<std::pair<std::shared_ptr<std::vector<uint8_t>>, std::pair<uint32_t, uint32_t>>> m_frameQueue;

    // Audio Queue (raw audio - antes do encoding)
    std::mutex m_audioQueueMutex;
    std::queue<std::pair<std::shared_ptr<std::vector<int16_t>>, std::pair<size_t, size_t>>> m_audioQueue;

    // Audio accumulator para acumular samples até ter um frame completo
    std::mutex m_audioAccumulatorMutex;
    std::vector<int16_t> m_audioAccumulator;

    // Threads
    std::thread m_serverThread;
    std::thread m_encodingThread; // Thread única para encoding

    int m_serverSocket = -1;
    std::atomic<uint32_t> m_clientCount{0};

    // Output buffer for clients
    std::mutex m_outputMutex; // Mutex para proteger m_clientSockets
    std::vector<int> m_clientSockets;

    // Header do formato MPEG-TS (enviado quando cliente se conecta)
    std::mutex m_headerMutex;
    std::vector<uint8_t> m_formatHeader;
    bool m_headerWritten = false;
};
