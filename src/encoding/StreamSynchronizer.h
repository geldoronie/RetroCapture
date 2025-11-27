#pragma once

#include <cstdint>
#include <vector>
#include <deque>
#include <memory>
#include <mutex>

/**
 * StreamSynchronizer - Classe responsável por sincronização de áudio e vídeo
 *
 * Gerencia buffers temporais de vídeo e áudio, calcula zonas de sincronização
 * e fornece dados sincronizados para encoding.
 */
class StreamSynchronizer
{
public:
    // Frame de vídeo com timestamp
    struct TimestampedFrame
    {
        std::shared_ptr<std::vector<uint8_t>> data;
        uint32_t width;
        uint32_t height;
        int64_t captureTimestampUs; // Timestamp absoluto de captura
        bool processed = false;     // Flag para marcar se já foi processado
    };

    // Chunk de áudio com timestamp
    struct TimestampedAudio
    {
        std::shared_ptr<std::vector<int16_t>> samples;
        size_t sampleCount;
        int64_t captureTimestampUs; // Timestamp absoluto de captura
        int64_t durationUs;         // Duração deste chunk em microssegundos
        bool processed = false;     // Flag para marcar se já foi processado
    };

    // Zona de sincronização (período onde vídeo e áudio estão sincronizados)
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

    StreamSynchronizer();
    ~StreamSynchronizer();

    // Configurar parâmetros de sincronização
    void setSyncTolerance(int64_t toleranceUs) { m_syncToleranceUs = toleranceUs; }
    void setMaxBufferTime(int64_t maxTimeUs) { m_maxBufferTimeUs = maxTimeUs; }
    void setMinBufferTime(int64_t minTimeUs) { m_minBufferTimeUs = minTimeUs; }

    // Adicionar frame de vídeo
    bool addVideoFrame(const uint8_t *data, uint32_t width, uint32_t height, int64_t captureTimestampUs);

    // Adicionar chunk de áudio
    bool addAudioChunk(const int16_t *samples, size_t sampleCount, int64_t captureTimestampUs, uint32_t sampleRate, uint32_t channels);

    // Calcular zona de sincronização
    SyncZone calculateSyncZone();

    // Obter frames de vídeo de uma zona de sincronização
    std::vector<TimestampedFrame> getVideoFrames(const SyncZone &zone);

    // Obter chunks de áudio de uma zona de sincronização
    std::vector<TimestampedAudio> getAudioChunks(const SyncZone &zone);

    // Marcar dados como processados
    void markVideoProcessed(size_t startIdx, size_t endIdx);
    void markAudioProcessed(size_t startIdx, size_t endIdx);

    // Limpar dados antigos (baseado em tempo)
    void cleanupOldData();

    // Limpar todos os buffers
    void clear();

    // Obter estatísticas
    size_t getVideoBufferSize() const;
    size_t getAudioBufferSize() const;
    int64_t getLatestVideoTimestamp() const { return m_latestVideoTimestampUs; }
    int64_t getLatestAudioTimestamp() const { return m_latestAudioTimestampUs; }

private:
    // Obter timestamp atual em microssegundos
    int64_t getTimestampUs() const;

    // Parâmetros de sincronização
    int64_t m_syncToleranceUs = 50 * 1000LL;    // 50ms de tolerância
    int64_t m_maxBufferTimeUs = 30 * 1000000LL; // 30 segundos máximo
    int64_t m_minBufferTimeUs = 0;              // 0ms - processar imediatamente

    // Buffers temporais ordenados por timestamp
    mutable std::mutex m_videoBufferMutex;
    std::deque<TimestampedFrame> m_videoBuffer;
    int64_t m_latestVideoTimestampUs = 0;
    int64_t m_firstVideoTimestampUs = 0;

    mutable std::mutex m_audioBufferMutex;
    std::deque<TimestampedAudio> m_audioBuffer;
    int64_t m_latestAudioTimestampUs = 0;
    int64_t m_firstAudioTimestampUs = 0;
};
