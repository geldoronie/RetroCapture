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
 */
class HTTPTSStreamer : public IStreamer {
public:
    HTTPTSStreamer();
    ~HTTPTSStreamer() override;
    
    std::string getType() const override { return "HTTP MPEG-TS"; }
    bool initialize(uint16_t port, uint32_t width, uint32_t height, uint32_t fps) override;
    bool start() override;
    void stop() override;
    bool isActive() const override;
    bool pushFrame(const uint8_t* data, uint32_t width, uint32_t height) override;
    bool pushAudio(const int16_t* samples, size_t sampleCount) override;
    std::string getStreamUrl() const override;
    uint32_t getClientCount() const override;
    void cleanup() override;
    
    // Additional configuration methods
    void setVideoBitrate(uint32_t bitrate) { m_videoBitrate = bitrate; }
    void setAudioBitrate(uint32_t bitrate) { m_audioBitrate = bitrate; }
    void setAudioFormat(uint32_t sampleRate, uint32_t channels);
    void setVideoCodec(const std::string& codecName); // "h264", "h265", "vp8", "vp9", etc
    void setAudioCodec(const std::string& codecName); // "aac", "mp3", "opus", etc
    void setAudioBufferSize(uint32_t frames) { m_audioBufferSizeFrames = frames; } // Tamanho do buffer em frames (override manual)
    void updateAudioBufferSize(); // Calcular automaticamente baseado no tempo de 1 frame de vídeo
    
    // Public for static callback
    int writeToClients(const uint8_t* buf, int buf_size);
    
private:
    void serverThread();
    void handleClient(int clientFd);
    void encodingThread();
    bool initializeFFmpeg();
    void cleanupFFmpeg();
    bool encodeVideoFrame(const uint8_t* rgbData, uint32_t width, uint32_t height);
    bool encodeAudioFrame(const int16_t* samples, size_t sampleCount);
    bool muxPacket(void* packet); // AVPacket* - using void* to avoid including FFmpeg headers in .h
    
    uint16_t m_port = 8080;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_fps = 30;
    uint32_t m_videoBitrate = 2000000; // 2 Mbps
    uint32_t m_audioBitrate = 128000;  // 128 kbps
    
    // Codec selection
    std::string m_videoCodecName = "h264"; // "h264", "h265", "vp8", "vp9"
    std::string m_audioCodecName = "aac";  // "aac", "mp3", "opus"
    
    // Audio format
    uint32_t m_sampleRate = 44100;
    uint32_t m_channels = 2;
    uint32_t m_audioBufferSizeFrames = 50; // Tamanho do buffer de áudio em frames (padrão: 50 = ~1 segundo a 48kHz)
    
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_running{false};
    std::thread m_serverThread;
    std::thread m_encodingThread;
    
    int m_serverSocket = -1;
    std::atomic<uint32_t> m_clientCount{0};
    
    // FFmpeg context (using void* to avoid including FFmpeg headers in .h)
    struct FFmpegContext {
        // Video
        void* videoCodecCtx = nullptr; // AVCodecContext*
        void* videoFrame = nullptr; // AVFrame*
        void* swsCtx = nullptr; // SwsContext*
        int64_t videoPts = 0;
        
        // Audio
        void* audioCodecCtx = nullptr; // AVCodecContext*
        void* audioFrame = nullptr; // AVFrame*
        void* swrCtx = nullptr; // SwrContext*
        int64_t audioPts = 0;
        int audioFrameSize = 0;
        
        // Clock para sincronização
        int64_t startTime = 0; // Tempo de início em microsegundos
        int64_t audioSamplesProcessed = 0; // Total de samples de áudio processados
        
        // Contadores incrementais para sincronização precisa
        // Usar contadores ao invés de tempo real para garantir sincronização perfeita
        int64_t videoFrameCount = 0; // Contador de frames de vídeo processados
        int64_t audioFrameCount = 0; // Contador de frames de áudio processados
        
        // Contadores para garantir DTS monotônico
        // Usar -1 como valor inicial (equivalente a AV_NOPTS_VALUE)
        int64_t lastVideoDts = -1;
        int64_t lastAudioDts = -1;
        
        // Muxing
        void* formatCtx = nullptr; // AVFormatContext*
        void* videoStream = nullptr; // AVStream*
        void* audioStream = nullptr; // AVStream*
        uint8_t* formatBuffer = nullptr;
        size_t formatBufferSize = 0;
    } m_ffmpeg;
    
    // Frame queues
    std::mutex m_frameMutex;
    std::queue<std::vector<uint8_t>> m_videoFrames;
    uint32_t m_frameWidth = 0;
    uint32_t m_frameHeight = 0;
    
    std::mutex m_audioMutex;
    std::queue<std::vector<int16_t>> m_audioSamples;
    std::vector<int16_t> m_audioBuffer; // Buffer para acumular samples até ter um frame completo
    
    // Output buffer for clients
    std::mutex m_outputMutex;
    std::vector<uint8_t> m_outputBuffer;
    std::vector<int> m_clientSockets;
};

