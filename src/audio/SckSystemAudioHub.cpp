#include "SckSystemAudioHub.h"

#ifdef __APPLE__

#include "AudioBus.h"
#include "../utils/Logger.h"

#include <chrono>
#include <ctime>
#include <vector>

namespace
{
// Inject silence in ~20 ms chunks, but only as a genuine safety net: once
// SCK is the producer it delivers continuous audio buffers (even during
// silence), so the keepalive must never fight real-audio jitter or flood
// at startup before the first real buffer arrives. We therefore only fill
// after a longer gap (~250 ms) with no real audio at all — covering a real
// SCK stall/pause, not normal per-buffer spacing.
constexpr int64_t kSilenceChunkMs = 20;
constexpr int64_t kSilenceGapUs   = 250'000; // 250 ms

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
    m_realSamplesWindow += sampleCount;
    // Is this buffer actually carrying sound, or did SCK hand us silence?
    // Peek a few samples so the telemetry can say "real audio but silent"
    // vs "genuinely no real audio reaching the hub".
    for (std::size_t i = 0; i < sampleCount; ++i)
    {
        if (samples[i] != 0) { m_nonZeroSamplesWindow++; break; }
    }
    m_consumer->push(samples, sampleCount);
}

void SckSystemAudioHub::requestSystemAudio(AudioBus *bus, uint32_t sampleRate, uint32_t channels)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_consumer = bus;
    m_rate     = sampleRate ? sampleRate : 48000;
    m_channels = channels ? channels : 2;
    // Treat the timeline as fresh so the keepalive waits a full gap before
    // assuming silence — this avoids flooding the bus with silence at
    // startup (which collided with the first real buffers and overflowed
    // the audio sync buffer).
    m_lastRealPushUs = monotonicUs();
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
    int64_t lastReportUs = monotonicUs();
    while (m_keepaliveRun.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(kSilenceChunkMs));

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_consumer) continue;

        const int64_t now = monotonicUs();
        const bool realFlowing =
            (m_lastRealPushUs != 0 && (now - m_lastRealPushUs) < kSilenceGapUs);

        if (!realFlowing)
        {
            // Fill one chunk of silence so the audio timeline keeps advancing.
            const size_t frames  = static_cast<size_t>(m_rate) * kSilenceChunkMs / 1000;
            const size_t samples = frames * m_channels;
            if (samples != 0)
            {
                if (silence.size() != samples) silence.assign(samples, 0);
                m_consumer->push(silence.data(), silence.size());
                m_silenceSamplesWindow += samples;
            }
            // Note: do NOT update m_lastRealPushUs — that tracks *real* audio
            // only, so the keepalive keeps filling while silence lasts.
        }

        // Telemetry: every ~2 s say what actually reached the bus, so we can
        // tell real audio from keepalive silence without guessing (#109).
        if (now - lastReportUs >= 2'000'000)
        {
            LOG_DEBUG("SckSystemAudioHub: last 2s — real=" +
                     std::to_string(m_realSamplesWindow) + " (nonzero buffers=" +
                     std::to_string(m_nonZeroSamplesWindow) + "), silence=" +
                     std::to_string(m_silenceSamplesWindow) +
                     ", screenActive=" + std::to_string(m_screenActive ? 1 : 0) +
                     ", ownStream=" + std::to_string(m_ownStream ? 1 : 0));
            m_realSamplesWindow = 0;
            m_nonZeroSamplesWindow = 0;
            m_silenceSamplesWindow = 0;
            lastReportUs = now;
        }
    }
}

#endif // __APPLE__
