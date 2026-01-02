#include "StreamSynchronizer.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <ctime>

StreamSynchronizer::StreamSynchronizer()
{
}

StreamSynchronizer::~StreamSynchronizer()
{
    clear();
}

int64_t StreamSynchronizer::getTimestampUs() const
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000LL + static_cast<int64_t>(ts.tv_nsec) / 1000LL;
}

bool StreamSynchronizer::addVideoFrame(const uint8_t *data, uint32_t width, uint32_t height, int64_t captureTimestampUs)
{
    if (!data || width == 0 || height == 0)
    {
        return false;
    }

    size_t expectedSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
    if (expectedSize == 0 || expectedSize > 100 * 1024 * 1024)
    {
        LOG_ERROR("StreamSynchronizer: Invalid frame size");
        return false;
    }

    TimestampedFrame frame;
    frame.data = std::make_shared<std::vector<uint8_t>>(data, data + expectedSize);
    frame.width = width;
    frame.height = height;
    frame.captureTimestampUs = captureTimestampUs;
    frame.processed = false;

    if (frame.data->size() != expectedSize)
    {
        LOG_ERROR("StreamSynchronizer: Frame data size mismatch");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_videoBufferMutex);
        if (m_videoBuffer.empty())
        {
            m_firstVideoTimestampUs = captureTimestampUs;
        }

        // Limitar tamanho do buffer para evitar vazamento de memória
        while (m_videoBuffer.size() >= m_maxVideoBufferSize)
        {
            // Remover frame mais antigo (mesmo que não processado)
            m_videoBuffer.pop_front();
        }

        m_videoBuffer.push_back(std::move(frame));
        m_latestVideoTimestampUs = std::max(m_latestVideoTimestampUs, captureTimestampUs);
    }

    // Não chamar cleanupOldData() aqui - deixar para o encodingThread fazer ocasionalmente
    // Isso evita remover dados muito agressivamente antes de serem processados
    // cleanupOldData() será chamado periodicamente pelo encodingThread

    return true;
}

bool StreamSynchronizer::addAudioChunk(const int16_t *samples, size_t sampleCount, int64_t captureTimestampUs, uint32_t sampleRate, uint32_t channels)
{
    if (!samples || sampleCount == 0)
    {
        return false;
    }

    // Calcular duração deste chunk de áudio
    // sampleCount é o número total de samples (todos os canais)
    // Para calcular a duração: samples_por_canal / sample_rate
    // samples_por_canal = sampleCount / channels
    // duração = (sampleCount / channels) * 1000000LL / sampleRate
    int64_t durationUs = (sampleCount * 1000000LL) / (sampleRate * channels);

    TimestampedAudio audio;
    audio.samples = std::make_shared<std::vector<int16_t>>(samples, samples + sampleCount);
    audio.sampleCount = sampleCount;
    audio.captureTimestampUs = captureTimestampUs;
    audio.durationUs = durationUs;
    audio.processed = false;

    {
        std::lock_guard<std::mutex> lock(m_audioBufferMutex);
        if (m_audioBuffer.empty())
        {
            m_firstAudioTimestampUs = captureTimestampUs;
        }

        // Limitar tamanho do buffer para evitar vazamento de memória
        while (m_audioBuffer.size() >= m_maxAudioBufferSize)
        {
            // Remover chunk mais antigo (mesmo que não processado)
            m_audioBuffer.pop_front();
        }

        m_audioBuffer.push_back(audio);
        m_latestAudioTimestampUs = std::max(m_latestAudioTimestampUs, captureTimestampUs);
    }

    // Não chamar cleanupOldData() aqui - deixar para o encodingThread fazer ocasionalmente
    // Isso evita remover dados muito agressivamente antes de serem processados
    // cleanupOldData() será chamado periodicamente pelo encodingThread

    return true;
}

StreamSynchronizer::SyncZone StreamSynchronizer::calculateSyncZone()
{
    std::lock_guard<std::mutex> videoLock(m_videoBufferMutex);
    std::lock_guard<std::mutex> audioLock(m_audioBufferMutex);

    // If either buffer is empty, return invalid zone
    // BUT: if audio is not required, we can still process video-only
    // This check is done in RecordingManager, so we return invalid here
    if (m_videoBuffer.empty() || m_audioBuffer.empty())
    {
        return SyncZone::invalid();
    }

    // Encontrar sobreposição temporal entre buffers
    int64_t videoStartUs = m_videoBuffer.front().captureTimestampUs;
    int64_t videoEndUs = m_videoBuffer.back().captureTimestampUs;

    int64_t audioStartUs = m_audioBuffer.front().captureTimestampUs;
    int64_t audioEndUs = m_audioBuffer.back().captureTimestampUs;

    // Calcular sobreposição
    int64_t overlapStartUs = std::max(videoStartUs, audioStartUs);
    int64_t overlapEndUs = std::min(videoEndUs, audioEndUs);

    // Verificar se há sobreposição
    // Usar tolerância para permitir processamento mesmo com pequeno descompasso
    // Isso evita perda de frames quando há pequena diferença temporal
    int64_t overlapDuration = overlapEndUs - overlapStartUs;
    int64_t toleranceUs = m_syncToleranceUs; // 50ms por padrão

    // Se não há sobreposição direta, verificar se estão próximos o suficiente
    if (overlapDuration <= 0)
    {
        // Calcular gap entre os buffers
        int64_t gapUs = (videoStartUs > audioEndUs) ? (videoStartUs - audioEndUs) : (audioStartUs - videoEndUs);

        // Se o gap é pequeno (dentro da tolerância), permitir processamento
        if (gapUs <= toleranceUs && gapUs >= 0)
        {
            // Criar zona de sincronização mesmo sem sobreposição direta
            overlapStartUs = std::min(videoStartUs, audioStartUs);
            overlapEndUs = std::max(videoEndUs, audioEndUs);
        }
        else
        {
            return SyncZone::invalid();
        }
    }

    // Zona sincronizada: processar desde o início da sobreposição até o final
    int64_t zoneStartUs = overlapStartUs;
    int64_t zoneEndUs = overlapEndUs;

    // Encontrar índices correspondentes nos buffers
    size_t videoStartIdx = 0;
    size_t videoEndIdx = 0;
    for (size_t i = 0; i < m_videoBuffer.size(); i++)
    {
        if (m_videoBuffer[i].captureTimestampUs >= zoneStartUs && videoStartIdx == 0)
        {
            videoStartIdx = i;
        }
        if (m_videoBuffer[i].captureTimestampUs <= zoneEndUs)
        {
            videoEndIdx = i + 1;
        }
    }

    size_t audioStartIdx = 0;
    size_t audioEndIdx = 0;
    for (size_t i = 0; i < m_audioBuffer.size(); i++)
    {
        if (m_audioBuffer[i].captureTimestampUs >= zoneStartUs && audioStartIdx == 0)
        {
            audioStartIdx = i;
        }
        if (m_audioBuffer[i].captureTimestampUs <= zoneEndUs)
        {
            audioEndIdx = i + 1;
        }
    }

    SyncZone zone;
    zone.startTimeUs = zoneStartUs;
    zone.endTimeUs = zoneEndUs;
    zone.videoStartIdx = videoStartIdx;
    zone.videoEndIdx = videoEndIdx;
    zone.audioStartIdx = audioStartIdx;
    zone.audioEndIdx = audioEndIdx;

    return zone;
}

std::vector<StreamSynchronizer::TimestampedFrame> StreamSynchronizer::getVideoFrames(const SyncZone &zone)
{
    std::lock_guard<std::mutex> lock(m_videoBufferMutex);
    std::vector<TimestampedFrame> frames;

    if (zone.videoEndIdx > zone.videoStartIdx && zone.videoEndIdx <= m_videoBuffer.size())
    {
        for (size_t i = zone.videoStartIdx; i < zone.videoEndIdx; i++)
        {
            frames.push_back(m_videoBuffer[i]);
        }
        
        // CRITICAL: Sort frames by timestamp to ensure correct playback order
        // Frames may arrive out of order due to capture timing, so we must sort them
        std::sort(frames.begin(), frames.end(), 
                  [](const TimestampedFrame &a, const TimestampedFrame &b) {
                      return a.captureTimestampUs < b.captureTimestampUs;
                  });
    }

    return frames;
}

std::vector<StreamSynchronizer::TimestampedAudio> StreamSynchronizer::getAudioChunks(const SyncZone &zone)
{
    std::lock_guard<std::mutex> lock(m_audioBufferMutex);
    std::vector<TimestampedAudio> chunks;

    if (zone.audioEndIdx > zone.audioStartIdx && zone.audioEndIdx <= m_audioBuffer.size())
    {
        for (size_t i = zone.audioStartIdx; i < zone.audioEndIdx; i++)
        {
            chunks.push_back(m_audioBuffer[i]);
        }
        
        // CRITICAL: Sort chunks by timestamp to ensure correct playback order
        // Audio chunks may arrive out of order, so we must sort them
        std::sort(chunks.begin(), chunks.end(), 
                  [](const TimestampedAudio &a, const TimestampedAudio &b) {
                      return a.captureTimestampUs < b.captureTimestampUs;
                  });
    }

    return chunks;
}

void StreamSynchronizer::markVideoProcessed(size_t startIdx, size_t endIdx)
{
    std::lock_guard<std::mutex> lock(m_videoBufferMutex);
    if (endIdx > startIdx && endIdx <= m_videoBuffer.size())
    {
        for (size_t i = startIdx; i < endIdx; i++)
        {
            m_videoBuffer[i].processed = true;
        }
    }
}

void StreamSynchronizer::markAudioProcessed(size_t startIdx, size_t endIdx)
{
    std::lock_guard<std::mutex> lock(m_audioBufferMutex);
    if (endIdx > startIdx && endIdx <= m_audioBuffer.size())
    {
        for (size_t i = startIdx; i < endIdx; i++)
        {
            m_audioBuffer[i].processed = true;
        }
    }
}

void StreamSynchronizer::cleanupOldData()
{
    // Calcular timestamp mais antigo permitido (janela temporal)
    int64_t oldestAllowedVideoUs = m_latestVideoTimestampUs - m_maxBufferTimeUs;
    int64_t oldestAllowedAudioUs = m_latestAudioTimestampUs - m_maxBufferTimeUs;

    // Remover apenas frames processados que estão fora da janela temporal
    // NÃO remover frames não processados mesmo se o buffer estiver grande
    // Isso evita perder dados antes de serem processados (causando skips)
    {
        std::lock_guard<std::mutex> lock(m_videoBufferMutex);
        while (!m_videoBuffer.empty())
        {
            const auto &frame = m_videoBuffer.front();
            bool isOld = frame.captureTimestampUs < oldestAllowedVideoUs;

            // Apenas remover se estiver antigo E já processado
            // Não remover frames não processados para evitar skips
            if (isOld && frame.processed)
            {
                m_videoBuffer.pop_front();
            }
            else
            {
                break;
            }
        }
    }

    // Mesma lógica para áudio - apenas remover chunks processados antigos
    {
        std::lock_guard<std::mutex> lock(m_audioBufferMutex);
        while (!m_audioBuffer.empty())
        {
            const auto &audio = m_audioBuffer.front();
            bool isOld = audio.captureTimestampUs < oldestAllowedAudioUs;

            // Apenas remover se estiver antigo E já processado
            // Não remover chunks não processados para evitar skips de áudio
            if (isOld && audio.processed)
            {
                m_audioBuffer.pop_front();
            }
            else
            {
                break;
            }
        }
    }
}

void StreamSynchronizer::clear()
{
    {
        std::lock_guard<std::mutex> lock(m_videoBufferMutex);
        m_videoBuffer.clear();
        m_latestVideoTimestampUs = 0;
        m_firstVideoTimestampUs = 0;
    }

    {
        std::lock_guard<std::mutex> lock(m_audioBufferMutex);
        m_audioBuffer.clear();
        m_latestAudioTimestampUs = 0;
        m_firstAudioTimestampUs = 0;
    }
}

size_t StreamSynchronizer::getVideoBufferSize() const
{
    std::lock_guard<std::mutex> lock(m_videoBufferMutex);
    return m_videoBuffer.size();
}

size_t StreamSynchronizer::getAudioBufferSize() const
{
    std::lock_guard<std::mutex> lock(m_audioBufferMutex);
    return m_audioBuffer.size();
}
