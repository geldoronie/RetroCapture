#pragma once

#include "IStreamer.h"
#include "WebPortal.h"
#include "HTTPServer.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <memory>
#include <cstdint>
#include <queue>
#include <deque>
#include <utility>
#include <string>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <chrono>
#include <condition_variable>
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
    void setH264Preset(const std::string &preset) { m_h264Preset = preset; }
    void setH265Preset(const std::string &preset) { m_h265Preset = preset; }
    void setH265Profile(const std::string &profile) { m_h265Profile = profile; }
    void setH265Level(const std::string &level) { m_h265Level = level; }
    void setVP8Speed(int speed) { m_vp8Speed = speed; }
    void setVP9Speed(int speed) { m_vp9Speed = speed; }

    // HTTPS configuration
    void enableHTTPS(bool enable) { m_enableHTTPS = enable; }
    void setSSLCertificatePath(const std::string &certPath, const std::string &keyPath);

    // Web Portal configuration
    void enableWebPortal(bool enable);
    bool isWebPortalEnabled() const { return m_webPortalEnabled; }

    // Obter caminhos dos certificados SSL encontrados
    std::string getFoundSSLCertificatePath() const { return m_foundSSLCertPath; }
    std::string getFoundSSLKeyPath() const { return m_foundSSLKeyPath; }

    // Public for static callback
    int writeToClients(const uint8_t *buf, int buf_size);

    // Estruturas para dados com timestamp de captura (declaradas antes de private para uso nas funções)
    struct TimestampedFrame
    {
        std::shared_ptr<std::vector<uint8_t>> data;
        uint32_t width;
        uint32_t height;
        int64_t captureTimestampUs; // Timestamp absoluto de captura (CLOCK_MONOTONIC)
        bool processed = false;     // Flag para marcar se já foi processado e enviado
    };

    struct TimestampedAudio
    {
        std::shared_ptr<std::vector<int16_t>> samples;
        size_t sampleCount;
        int64_t captureTimestampUs; // Timestamp absoluto de captura (CLOCK_MONOTONIC)
        int64_t durationUs;         // Duração deste chunk em microssegundos
        bool processed = false;     // Flag para marcar se já foi processado e enviado
    };

    // Zona de sincronização
    struct SyncZone
    {
        int64_t startTimeUs;  // Início da zona sincronizada
        int64_t endTimeUs;    // Fim da zona sincronizada
        size_t videoStartIdx; // Índice inicial no buffer de vídeo
        size_t videoEndIdx;   // Índice final no buffer de vídeo
        size_t audioStartIdx; // Índice inicial no buffer de áudio
        size_t audioEndIdx;   // Índice final no buffer de áudio

        bool isValid() const
        {
            return startTimeUs < endTimeUs &&
                   videoEndIdx > videoStartIdx &&
                   audioEndIdx > audioStartIdx;
        }

        static SyncZone invalid()
        {
            SyncZone zone;
            zone.startTimeUs = 0;
            zone.endTimeUs = 0;
            return zone;
        }
    };

private:
    void serverThread();
    void handleClient(int clientFd);
    void serveHLSPlaylist(int clientFd, const std::string &basePrefix = "");    // Servir playlist M3U8 para HLS
    void serveHLSSegment(int clientFd, int segmentIndex);                       // Servir segmento HLS
    void send404(int clientFd);                                                 // Enviar resposta 404
    std::string generateM3U8Playlist(const std::string &basePrefix = "") const; // Gerar playlist M3U8 dinâmica
    void encodingThread();                                                      // Thread para encoding com sincronização baseada em timestamps
    void hlsSegmentThread();                                                    // Thread para segmentar stream em HLS
    SyncZone calculateSyncZone();                                               // Calcular zona de sincronização entre vídeo e áudio
    void cleanupOldData();                                                      // Limpar dados antigos baseado em tempo
    int64_t getTimestampUs() const;                                             // Obter timestamp atual em microssegundos
    bool initializeFFmpeg();
    bool initializeVideoCodec();
    bool initializeAudioCodec();
    bool initializeMuxers();
    void cleanupFFmpeg();
    void flushCodecs();
    bool encodeVideoFrame(const uint8_t *rgbData, uint32_t width, uint32_t height, int64_t captureTimestampUs);
    bool encodeAudioFrame(const int16_t *samples, size_t sampleCount, int64_t captureTimestampUs);
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
    std::string m_h264Preset = "veryfast"; // Preset H.264 configurável via UI
    std::string m_h265Preset = "veryfast"; // Preset H.265 configurável via UI
    std::string m_h265Profile = "main";    // Profile H.265: "main" (8-bit) ou "main10" (10-bit)
    std::string m_h265Level = "auto";      // Level H.265: "auto", "1", "2", "2.1", "3", "3.1", "4", "4.1", "5", "5.1", "5.2", "6", "6.1", "6.2"
    int m_vp8Speed = 12;                   // Speed VP8: 0-16 (0 = melhor qualidade, 16 = mais rápido, 12 = bom para streaming)
    int m_vp9Speed = 6;                    // Speed VP9: 0-9 (0 = melhor qualidade, 9 = mais rápido, 6 = bom para streaming)

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

    // Cache para dimensões do SwsContext (para recriar quando necessário)
    uint32_t m_swsSrcWidth = 0;
    uint32_t m_swsSrcHeight = 0;
    uint32_t m_swsDstWidth = 0;
    uint32_t m_swsDstHeight = 0;

    std::atomic<bool> m_active{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequest{false}; // Flag para solicitar parada das threads
    std::atomic<bool> m_cleanedUp{false};

    // Configuração de buffer temporal
    static constexpr int64_t MAX_BUFFER_TIME_US = 30 * 1000000LL; // 30 segundos máximo - buffer grande para evitar perda de frames
    static constexpr int64_t MIN_BUFFER_TIME_US = 0;              // 0ms - processar imediatamente quando há qualquer sobreposição (para 60fps)
    static constexpr int64_t SYNC_TOLERANCE_US = 50 * 1000LL;     // 50ms de tolerância para sincronização

    // Buffers temporais ordenados por timestamp de captura
    std::mutex m_videoBufferMutex;
    std::deque<TimestampedFrame> m_timestampedVideoBuffer;
    int64_t m_latestVideoTimestampUs = 0;
    int64_t m_firstVideoTimestampUs = 0; // Primeiro timestamp de vídeo (para calcular PTS relativo)

    std::mutex m_audioBufferMutex;
    std::deque<TimestampedAudio> m_timestampedAudioBuffer;
    int64_t m_latestAudioTimestampUs = 0;
    int64_t m_firstAudioTimestampUs = 0; // Primeiro timestamp de áudio (para calcular PTS relativo)

    // Audio accumulator para acumular samples até ter um frame completo do codec
    // (usado internamente no encodeAudioFrame)
    std::mutex m_audioAccumulatorMutex;
    std::vector<int16_t> m_audioAccumulator;

    // Rastreamento de último PTS/DTS usado para garantir monotonicidade
    // AV_NOPTS_VALUE = 0x8000000000000000 (não podemos usar no header, então usamos -1 como valor inválido)
    std::mutex m_ptsMutex;
    int64_t m_lastVideoFramePTS = -1; // Último PTS usado no frame (antes de enviar ao codec)
    int64_t m_lastVideoPTS = -1;      // Último PTS do pacote (depois do codec)
    int64_t m_lastVideoDTS = -1;
    int64_t m_lastAudioFramePTS = -1; // Último PTS usado no frame (antes de enviar ao codec)
    int64_t m_lastAudioPTS = -1;      // Último PTS do pacote (depois do codec)
    int64_t m_lastAudioDTS = -1;

    // Contador de frames para keyframes periódicos
    int64_t m_videoFrameCount = 0;

    // Detecção de dessincronização e recuperação
    int m_desyncFrameCount = 0; // Contador de frames dessincronizados consecutivos

    // Mutexes para sincronização
    std::mutex m_muxMutex;    // Proteger av_interleaved_write_frame (não é thread-safe)
    std::mutex m_outputMutex; // Proteger lista de clientes e writeToClients
    std::mutex m_headerMutex; // Proteger m_formatHeader

    // Threads
    std::thread m_serverThread;
    std::thread m_encodingThread; // Thread única para encoding

    // HTTP/HTTPS Server
    HTTPServer m_httpServer;
    bool m_enableHTTPS = false;
    std::string m_sslCertPath;
    std::string m_sslKeyPath;
    std::string m_foundSSLCertPath; // Caminho real do certificado encontrado (após busca)
    std::string m_foundSSLKeyPath;  // Caminho real da chave encontrada (após busca)

    std::atomic<uint32_t> m_clientCount{0};

    // Output buffer for clients
    std::vector<int> m_clientSockets;

    // Header do formato MPEG-TS (enviado quando cliente se conecta)
    std::vector<uint8_t> m_formatHeader;
    bool m_headerWritten = false;

    // Web Portal - responsável por servir a página web
    WebPortal m_webPortal;
    bool m_webPortalEnabled = true; // Habilitado por padrão

    // HLS (HTTP Live Streaming) support
    static constexpr int HLS_SEGMENT_DURATION_SEC = 2; // Duração de cada segmento em segundos
    static constexpr int HLS_SEGMENT_COUNT = 5;        // Número de segmentos a manter na playlist
    struct HLSSegment
    {
        std::vector<uint8_t> data;
        int64_t timestampUs;
        int index;
    };
    mutable std::mutex m_hlsMutex;
    std::deque<HLSSegment> m_hlsSegments; // Segmentos HLS (circular buffer)
    std::vector<uint8_t> m_hlsBuffer;     // Buffer para acumular dados MPEG-TS antes de criar segmento
    int m_hlsSegmentIndex = 0;            // Contador de segmentos
    int64_t m_hlsLastSegmentTimeUs = 0;   // Timestamp do último segmento criado
    std::thread m_hlsSegmentThread;       // Thread para criar segmentos
};
