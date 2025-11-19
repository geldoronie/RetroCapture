#pragma once

#include "IStreamer.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <memory>
#include <cstdint>
#include <queue>

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
    void cleanupFFmpeg();
    void flushCodecs();
    bool encodeVideoFrame(const uint8_t *rgbData, uint32_t width, uint32_t height);
    bool encodeAudioFrame(const int16_t *samples, size_t sampleCount);
    bool muxPacket(void *packet);

    uint16_t m_port = 8080;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_fps = 60;               // Taxa de frames configurada via initialize() - NUNCA hardcoded nos cálculos
    uint32_t m_videoBitrate = 2000000; // 2 Mbps
    uint32_t m_audioBitrate = 128000;  // 128 kbps

    // Codec selection
    std::string m_videoCodecName = "h264";
    std::string m_audioCodecName = "aac";

    // Audio format
    uint32_t m_sampleRate = 44100;
    uint32_t m_channels = 2;

    std::atomic<bool> m_active{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cleanedUp{false};
    std::thread m_serverThread;
    std::thread m_encodingThread; // Thread única para encoding

    int m_serverSocket = -1;
    std::atomic<uint32_t> m_clientCount{0};

    // FFmpeg context (using void* to avoid including FFmpeg headers in .h)
    struct FFmpegContext
    {
        // Video
        void *videoCodecCtx = nullptr; // AVCodecContext*
        void *videoFrame = nullptr;    // AVFrame*
        void *swsCtx = nullptr;        // SwsContext*
        int64_t videoPts = 0;

        // Audio
        void *audioCodecCtx = nullptr; // AVCodecContext*
        void *audioFrame = nullptr;    // AVFrame*
        void *swrCtx = nullptr;        // SwrContext*
        int64_t audioPts = 0;
        int audioFrameSize = 0;

        // Contadores incrementais simples para PTS
        int64_t videoFrameCount = 0;  // Contador de frames de vídeo processados
        int64_t audioSampleCount = 0; // Contador total de samples de áudio processados

        // DTS monotônico
        int64_t lastVideoDts = -1;
        int64_t lastAudioDts = -1;

        // Flag para forçar keyframe
        bool forceKeyFrame = true;

        // Muxing
        void *formatCtx = nullptr;   // AVFormatContext*
        void *videoStream = nullptr; // AVStream*
        void *audioStream = nullptr; // AVStream*
        uint8_t *formatBuffer = nullptr;
        size_t formatBufferSize = 0;
    } m_ffmpeg;

    // BUFFERS INDEPENDENTES (OBS style)
    // Buffer de vídeo: fila simples de frames
    struct VideoFrame
    {
        std::vector<uint8_t> data;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    std::mutex m_videoMutex;
    std::queue<VideoFrame> m_videoQueue;
    static constexpr size_t MAX_VIDEO_QUEUE_SIZE = 120; // Máximo de frames na fila (aumentado para evitar descarte)

    // Buffer de áudio: acumulador contínuo de samples
    std::mutex m_audioMutex;
    std::vector<int16_t> m_audioBuffer;                           // Buffer contínuo de samples
    static constexpr size_t MAX_AUDIO_BUFFER_SAMPLES = 44100 * 2; // ~1 segundo a 44.1kHz stereo

    // Mutex para proteger o muxing (av_interleaved_write_frame não é thread-safe)
    std::mutex m_muxMutex;

    // Output buffer for clients
    std::mutex m_outputMutex;
    std::vector<uint8_t> m_outputBuffer;
    std::vector<int> m_clientSockets;
};
