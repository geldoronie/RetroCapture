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

    // CRÍTICO: Validar tamanho antes de copiar
    // RGB24 = 3 bytes por pixel, total = width * height * 3
    size_t expectedSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
    if (expectedSize == 0 || expectedSize > 100 * 1024 * 1024) // Máximo 100MB por frame
    {
        LOG_ERROR("StreamSynchronizer: Invalid frame size: " + std::to_string(width) + "x" + std::to_string(height));
        return false;
    }

    TimestampedFrame frame;
    // CRÍTICO: Copiar dados RGB24 (3 bytes por pixel, sem padding)
    // O código assume que os dados vêm sem padding/stride do Application.cpp
    frame.data = std::make_shared<std::vector<uint8_t>>(data, data + expectedSize);
    frame.width = width;
    frame.height = height;
    frame.captureTimestampUs = captureTimestampUs;
    frame.processed = false;

    // Validar que a cópia foi bem-sucedida (verificar primeiros e últimos bytes)
    if (frame.data->size() != expectedSize)
    {
        LOG_ERROR("StreamSynchronizer: Frame data size mismatch: expected " +
                  std::to_string(expectedSize) + ", got " + std::to_string(frame.data->size()));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_videoBufferMutex);
        if (m_videoBuffer.empty())
        {
            m_firstVideoTimestampUs = captureTimestampUs;
        }
        m_videoBuffer.push_back(std::move(frame));
        m_latestVideoTimestampUs = std::max(m_latestVideoTimestampUs, captureTimestampUs);
    }

    // CRÍTICO: Limpar dados antigos FORA do lock para evitar deadlock
    // cleanupOldData() precisa adquirir m_audioBufferMutex, e se a thread principal
    // já está segurando esse lock e tentando adquirir m_videoBufferMutex, teríamos deadlock
    // Chamar cleanupOldData() fora do lock evita esse problema
    static size_t cleanupCounter = 0;
    if (++cleanupCounter >= 60)
    {
        cleanupOldData();
        cleanupCounter = 0;
    }

    return true;
}

bool StreamSynchronizer::addAudioChunk(const int16_t *samples, size_t sampleCount, int64_t captureTimestampUs, uint32_t sampleRate, uint32_t channels)
{
    if (!samples || sampleCount == 0)
    {
        return false;
    }

    // Calcular duração deste chunk de áudio
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
        m_audioBuffer.push_back(audio);
        m_latestAudioTimestampUs = std::max(m_latestAudioTimestampUs, captureTimestampUs);
    }

    // CRÍTICO: Limpar dados antigos FORA do lock para evitar deadlock
    // cleanupOldData() precisa adquirir m_videoBufferMutex, e se a thread de streaming
    // já está segurando esse lock e tentando adquirir m_audioBufferMutex, teríamos deadlock
    // Chamar cleanupOldData() fora do lock evita esse problema
    cleanupOldData();

    return true;
}

StreamSynchronizer::SyncZone StreamSynchronizer::calculateSyncZone()
{
    std::lock_guard<std::mutex> videoLock(m_videoBufferMutex);
    std::lock_guard<std::mutex> audioLock(m_audioBufferMutex);

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
    if (overlapEndUs <= overlapStartUs)
    {
        return SyncZone::invalid();
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
    {
        std::lock_guard<std::mutex> lock(m_videoBufferMutex);
        while (!m_videoBuffer.empty())
        {
            const auto &frame = m_videoBuffer.front();
            if (frame.captureTimestampUs >= oldestAllowedVideoUs)
            {
                break;
            }
            if (frame.processed)
            {
                m_videoBuffer.pop_front();
            }
            else
            {
                // Frame não processado - manter mesmo que antigo
                break;
            }
        }
    }

    // Mesma lógica para áudio
    {
        std::lock_guard<std::mutex> lock(m_audioBufferMutex);
        while (!m_audioBuffer.empty())
        {
            const auto &audio = m_audioBuffer.front();
            if (audio.captureTimestampUs >= oldestAllowedAudioUs)
            {
                break;
            }
            if (audio.processed)
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
