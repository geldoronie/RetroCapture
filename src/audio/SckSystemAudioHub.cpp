#include "SckSystemAudioHub.h"

#ifdef __APPLE__

#include "AudioBus.h"
#include "../utils/Logger.h"

SckSystemAudioHub &SckSystemAudioHub::instance()
{
    static SckSystemAudioHub hub;
    return hub;
}

void SckSystemAudioHub::requestSystemAudio(AudioBus *bus, uint32_t sampleRate, uint32_t channels)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_consumer = bus;
    m_rate     = sampleRate;
    m_channels = channels;

    if (m_screenActive)
    {
        // Screen capture is already producing audio — route it through
        // onScreenAudio(); no separate stream (would conflict).
        LOG_INFO("SckSystemAudioHub: routing screen-capture audio to system-audio source");
        return;
    }
    // No screen stream → run our own audio-only SCStream (the case that
    // works on its own, e.g. camera + system audio).
    if (!m_ownStream)
    {
        m_ownStream = std::make_unique<SystemAudioCaptureSCK>();
        if (!m_ownStream->start(bus, sampleRate, channels))
        {
            m_ownStream.reset();
            LOG_WARN("SckSystemAudioHub: audio-only SCStream failed to start");
        }
    }
}

void SckSystemAudioHub::releaseSystemAudio()
{
    std::unique_ptr<SystemAudioCaptureSCK> toStop;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_consumer = nullptr;
        toStop = std::move(m_ownStream); // stop outside the lock
    }
    if (toStop) toStop->stop();
}

void SckSystemAudioHub::setScreenProducerActive(bool active)
{
    std::unique_ptr<SystemAudioCaptureSCK> toStop;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_screenActive = active;
        if (active)
        {
            // Screen now provides audio — drop our redundant own stream
            // (two SCStreams conflict). Its audio arrives via onScreenAudio.
            toStop = std::move(m_ownStream);
        }
        else if (m_consumer && !m_ownStream)
        {
            // Screen stopped but a consumer still wants system audio —
            // bring our own audio-only stream back up.
            m_ownStream = std::make_unique<SystemAudioCaptureSCK>();
            if (!m_ownStream->start(m_consumer, m_rate, m_channels))
            {
                m_ownStream.reset();
            }
        }
    }
    if (toStop) toStop->stop();
}

void SckSystemAudioHub::onScreenAudio(const int16_t *samples, std::size_t sampleCount)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // Only forward while the screen stream is the active producer (when
    // it isn't, the own audio-only stream pushes to the bus directly).
    if (m_consumer && m_screenActive && samples && sampleCount)
    {
        m_consumer->push(samples, sampleCount);
    }
}

#endif // __APPLE__
