#include "SckSystemAudioHub.h"

#ifdef __APPLE__

#include "AudioBus.h"
#include "../utils/Logger.h"

#include <chrono>
#include <ctime>
#include <vector>

namespace
{
// Inject silence in ~20 ms chunks; treat the timeline as silent once no
// real audio has arrived for ~60 ms (three chunks). Continuous real audio
// keeps m_lastRealPushUs fresh so the keepalive never fires mid-playback.
constexpr int64_t kSilenceChunkMs = 20;
constexpr int64_t kSilenceGapUs   = 60'000; // 60 ms

int64_t monotonicUs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000LL +
           static_cast<int64_t>(ts.tv_nsec) / 1'000LL;
}
} // namespace

SckSystemAudioHub &SckSystemAudioHub::instance()
{
    static SckSystemAudioHub hub;
    return hub;
}

SckSystemAudioHub::~SckSystemAudioHub()
{
    stopKeepaliveAndJoin();
}

void SckSystemAudioHub::forwardRealAudioLocked(const int16_t *samples, std::size_t sampleCount)
{
    if (!m_consumer || !samples || !sampleCount) return;
    if (!m_loggedFirstReal)
    {
        m_loggedFirstReal = true;
        LOG_INFO("SckSystemAudioHub: first real system-audio samples reached the bus");
    }
    m_lastRealPushUs = monotonicUs();
    m_consumer->push(samples, sampleCount);
}

void SckSystemAudioHub::requestSystemAudio(AudioBus *bus, uint32_t sampleRate, uint32_t channels)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_consumer = bus;
    m_rate     = sampleRate ? sampleRate : 48000;
    m_channels = channels ? channels : 2;
    // Reset recency so the keepalive starts filling immediately until the
    // first real buffer (if any) arrives.
    m_lastRealPushUs = 0;
    m_loggedFirstReal = false;

    if (m_screenActive)
    {
        // Screen capture is already producing audio — route it through
        // onScreenAudio(); no separate stream (would conflict).
        LOG_INFO("SckSystemAudioHub: routing screen-capture audio to system-audio source");
    }
    else if (!m_ownStream)
    {
        // No screen stream → run our own audio-only SCStream (the case that
        // works on its own, e.g. camera + system audio).
        m_ownStream = std::make_unique<SystemAudioCaptureSCK>();
        if (!m_ownStream->start(
                [this](const int16_t *s, std::size_t n) {
                    std::lock_guard<std::mutex> l(m_mutex);
                    forwardRealAudioLocked(s, n);
                },
                m_rate, m_channels))
        {
            m_ownStream.reset();
            LOG_WARN("SckSystemAudioHub: audio-only SCStream failed to start");
        }
    }

    startKeepaliveLocked();
}

void SckSystemAudioHub::releaseSystemAudio()
{
    std::unique_ptr<SystemAudioCaptureSCK> toStop;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_consumer = nullptr;
        toStop = std::move(m_ownStream); // stop outside the lock
    }
    stopKeepaliveAndJoin();
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
            if (!m_ownStream->start(
                    [this](const int16_t *s, std::size_t n) {
                        std::lock_guard<std::mutex> l(m_mutex);
                        forwardRealAudioLocked(s, n);
                    },
                    m_rate, m_channels))
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
    // it isn't, the own audio-only stream pushes through forwardRealAudio).
    if (m_screenActive)
    {
        forwardRealAudioLocked(samples, sampleCount);
    }
}

void SckSystemAudioHub::startKeepaliveLocked()
{
    if (m_keepaliveRun.load()) return;
    m_keepaliveRun.store(true);
    m_keepalive = std::thread(&SckSystemAudioHub::keepaliveLoop, this);
}

void SckSystemAudioHub::stopKeepaliveAndJoin()
{
    if (m_keepaliveRun.exchange(false))
    {
        if (m_keepalive.joinable()) m_keepalive.join();
    }
}

void SckSystemAudioHub::keepaliveLoop()
{
    std::vector<int16_t> silence;
    while (m_keepaliveRun.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(kSilenceChunkMs));

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_consumer) continue;

        const int64_t now = monotonicUs();
        if (m_lastRealPushUs != 0 && (now - m_lastRealPushUs) < kSilenceGapUs)
        {
            continue; // real audio is flowing; don't double-fill
        }

        // Fill one chunk of silence so the audio timeline keeps advancing.
        const size_t frames  = static_cast<size_t>(m_rate) * kSilenceChunkMs / 1000;
        const size_t samples = frames * m_channels;
        if (samples == 0) continue;
        if (silence.size() != samples) silence.assign(samples, 0);
        m_consumer->push(silence.data(), silence.size());
        // Note: do NOT update m_lastRealPushUs — that tracks *real* audio
        // only, so the keepalive keeps filling for as long as silence lasts.
    }
}

#endif // __APPLE__
