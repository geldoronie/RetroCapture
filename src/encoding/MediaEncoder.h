#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>

/**
 * MediaEncoder - Classe responsável por encoding de vídeo e áudio
 *
 * Recebe dados brutos (RGB para vídeo, int16 para áudio) e produz
 * pacotes codificados prontos para muxing.
 */
class MediaEncoder
{
public:
    // Configuração de vídeo
    struct VideoConfig
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t fps = 60;
        uint32_t bitrate = 2000000; // 2 Mbps padrão
        std::string codec = "h264"; // "h264", "h265", "vp8", "vp9"
        std::string preset = "veryfast";
        std::string profile = "baseline"; // Para H.264
        std::string h265Profile = "main"; // Para H.265
        std::string h265Level = "auto";   // Para H.265
        int vp8Speed = 12;                // 0-16
        int vp9Speed = 6;                 // 0-9
    };

    // Configuração de áudio
    struct AudioConfig
    {
        uint32_t sampleRate = 44100;
        uint32_t channels = 2;
        uint32_t bitrate = 128000; // 128 kbps padrão
        std::string codec = "aac";
    };

    // Pacote codificado (vídeo ou áudio)
    struct EncodedPacket
    {
        std::vector<uint8_t> data;
        int64_t pts = -1;               // Presentation Timestamp (-1 = AV_NOPTS_VALUE)
        int64_t dts = -1;               // Decode Timestamp (-1 = AV_NOPTS_VALUE)
        bool isKeyframe = false;        // Apenas para vídeo
        bool isVideo = true;            // true = vídeo, false = áudio
        int64_t captureTimestampUs = 0; // Timestamp original de captura
    };

    MediaEncoder();
    ~MediaEncoder();

    // Inicializar encoder com configurações
    bool initialize(const VideoConfig &videoConfig, const AudioConfig &audioConfig);

    // Encoding de vídeo: RGB → YUV → codec
    // Retorna true se frame foi enviado ao codec (pode gerar 0 ou mais pacotes)
    bool encodeVideo(const uint8_t *rgbData, uint32_t width, uint32_t height,
                     int64_t captureTimestampUs, std::vector<EncodedPacket> &packets);

    // Encoding de áudio: int16 → float planar → codec
    // Retorna true se samples foram processados (pode gerar 0 ou mais pacotes)
    bool encodeAudio(const int16_t *samples, size_t sampleCount,
                     int64_t captureTimestampUs, std::vector<EncodedPacket> &packets);

    // Flush codecs - processar frames pendentes
    void flush(std::vector<EncodedPacket> &packets);

    // Limpar recursos
    void cleanup();

    // Verificar se está inicializado
    bool isInitialized() const { return m_initialized; }

    // Obter configurações atuais
    const VideoConfig &getVideoConfig() const { return m_videoConfig; }
    const AudioConfig &getAudioConfig() const { return m_audioConfig; }

    // Obter codec contexts (para MediaMuxer configurar streams)
    void *getVideoCodecContext() const { return m_videoCodecContext; }
    void *getAudioCodecContext() const { return m_audioCodecContext; }

    // Contador de frames (para keyframes periódicos)
    int64_t getVideoFrameCount() const { return m_videoFrameCount; }
    void resetVideoFrameCount() { m_videoFrameCount = 0; }

private:
    // Inicialização de codecs
    bool initializeVideoCodec();
    bool initializeAudioCodec();

    // Conversão de formatos
    bool convertRGBToYUV(const uint8_t *rgbData, uint32_t width, uint32_t height, void *videoFrame);
    bool convertInt16ToFloatPlanar(const int16_t *samples, size_t sampleCount, void *audioFrame, size_t outputSamples);

    // Processar pacotes do codec
    bool receiveVideoPackets(std::vector<EncodedPacket> &packets, int64_t captureTimestampUs);
    bool receiveAudioPackets(std::vector<EncodedPacket> &packets, int64_t captureTimestampUs);

    // Calcular PTS baseado em timestamp de captura
    int64_t calculateVideoPTS(int64_t captureTimestampUs);
    int64_t calculateAudioPTS(int64_t captureTimestampUs, size_t sampleCount);

    // Garantir monotonicidade de PTS/DTS
    void ensureMonotonicPTS(int64_t &pts, int64_t &dts, bool isVideo);

    // Configurações
    VideoConfig m_videoConfig;
    AudioConfig m_audioConfig;
    bool m_initialized = false;

    // Codec contexts (void* para evitar incluir headers FFmpeg no .h)
    void *m_videoCodecContext = nullptr; // AVCodecContext*
    void *m_audioCodecContext = nullptr; // AVCodecContext*

    // FFmpeg conversion contexts
    void *m_swsContext = nullptr; // SwsContext* (RGB to YUV)
    void *m_swrContext = nullptr; // SwrContext* (int16 to float planar)
    void *m_videoFrame = nullptr; // AVFrame* (para encoding de vídeo)
    void *m_audioFrame = nullptr; // AVFrame* (para encoding de áudio)

    // Cache para dimensões do SwsContext
    uint32_t m_swsSrcWidth = 0;
    uint32_t m_swsSrcHeight = 0;
    uint32_t m_swsDstWidth = 0;
    uint32_t m_swsDstHeight = 0;

    // Timestamps de referência (primeiro frame/chunk)
    int64_t m_firstVideoTimestampUs = 0;
    int64_t m_firstAudioTimestampUs = 0;
    bool m_firstVideoTimestampSet = false;
    bool m_firstAudioTimestampSet = false;

    // Rastreamento de PTS/DTS para garantir monotonicidade
    std::mutex m_ptsMutex;
    int64_t m_lastVideoPTS = -1;
    int64_t m_lastVideoDTS = -1;
    int64_t m_lastVideoFramePTS = -1; // PTS do frame antes de enviar ao codec
    int64_t m_lastAudioPTS = -1;
    int64_t m_lastAudioDTS = -1;
    int64_t m_lastAudioFramePTS = -1; // PTS do frame antes de enviar ao codec

    // Contador de frames para keyframes periódicos
    int64_t m_videoFrameCount = 0;

    // Audio accumulator para acumular samples até ter um frame completo
    std::mutex m_audioAccumulatorMutex;
    std::vector<int16_t> m_audioAccumulator;

    // Detecção de dessincronização
    int m_desyncFrameCount = 0;
};
