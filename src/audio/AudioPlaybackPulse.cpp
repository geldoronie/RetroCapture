#include "AudioPlaybackPulse.h"

#ifdef __linux__

#include "../utils/Logger.h"

#include <pulse/simple.h>
#include <pulse/error.h>

AudioPlaybackPulse::AudioPlaybackPulse() = default;

AudioPlaybackPulse::~AudioPlaybackPulse()
{
    close();
}

bool AudioPlaybackPulse::open(uint32_t sampleRate, uint32_t channels)
{
    // close() takes the exclusive lock itself, so release before
    // calling it to avoid self-deadlock, then re-acquire for the
    // open path.
    if (m_stream)
    {
        close();
    }
    std::unique_lock<std::shared_mutex> lock(m_streamMutex);
    if (sampleRate == 0 || channels == 0 || channels > 8)
    {
        LOG_ERROR("AudioPlaybackPulse::open — invalid format");
        return false;
    }

    pa_sample_spec spec;
    spec.format   = PA_SAMPLE_FLOAT32LE;
    spec.rate     = sampleRate;
    spec.channels = static_cast<uint8_t>(channels);

    // Tight buffering: small target length keeps latency low so video
    // gating against the audio clock doesn't have to compensate for a
    // huge hardware buffer. 50 ms is comfortable above network jitter
    // without making A/V sync look perceptibly behind.
    pa_buffer_attr attr;
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength   = pa_usec_to_bytes(50 * 1000, &spec);
    attr.prebuf    = static_cast<uint32_t>(-1);
    attr.minreq    = static_cast<uint32_t>(-1);
    attr.fragsize  = static_cast<uint32_t>(-1);

    int err = 0;
    m_stream = pa_simple_new(
        nullptr,                  // default server
        "RetroCapture",
        PA_STREAM_PLAYBACK,
        nullptr,                  // default device
        "Remote stream",
        &spec,
        nullptr,                  // default channel map
        &attr,
        &err);
    if (!m_stream)
    {
        LOG_ERROR(std::string("AudioPlaybackPulse: pa_simple_new failed — ") + pa_strerror(err));
        return false;
    }

    m_sampleRate    = sampleRate;
    m_channels      = channels;
    {
        std::lock_guard<std::mutex> lock(m_clockMutex);
        m_lastSubmittedPtsUs = 0;
        m_anySubmitted       = false;
    }
    LOG_INFO("AudioPlaybackPulse: opened " + std::to_string(sampleRate) +
             " Hz x " + std::to_string(channels) + " ch");
    return true;
}

void AudioPlaybackPulse::close()
{
    // Exclusive: wait for any in-flight pa_simple_* (submit / flush /
    // getClockUs / isOpen) to return before we destroy the stream.
    // Without this, pa_simple_free races with libpulse-simple's
    // internal mainloop mutex and aborts.
    std::unique_lock<std::shared_mutex> lock(m_streamMutex);
    if (m_stream)
    {
        // Best-effort drain so the user doesn't hear a click on
        // disconnect. Ignored on error — we're tearing down anyway.
        int err = 0;
        pa_simple_drain(m_stream, &err);
        pa_simple_free(m_stream);
        m_stream = nullptr;
    }
    m_sampleRate = 0;
    m_channels   = 0;
    std::lock_guard<std::mutex> clockLock(m_clockMutex);
    m_lastSubmittedPtsUs = 0;
    m_anySubmitted       = false;
}

size_t AudioPlaybackPulse::submit(const float *interleaved,
                                  size_t sampleCount,
                                  int64_t firstPtsUs)
{
    if (!interleaved || sampleCount == 0) return 0;

    std::shared_lock<std::shared_mutex> streamLock(m_streamMutex);
    if (!m_stream) return 0;

    const size_t bytes = sampleCount * m_channels * sizeof(float);
    int err = 0;
    if (pa_simple_write(m_stream, interleaved, bytes, &err) < 0)
    {
        static int logCount = 0;
        if (logCount++ < 3)
        {
            LOG_WARN(std::string("AudioPlaybackPulse: pa_simple_write failed — ") + pa_strerror(err));
        }
        return 0;
    }

    // Advance the playback clock: the END of this submission is at
    // firstPtsUs + duration. getClockUs returns the END minus the
    // current hardware latency, which approximates 'PTS being heard
    // right now'.
    const int64_t durationUs = (static_cast<int64_t>(sampleCount) * 1'000'000LL) /
                                static_cast<int64_t>(m_sampleRate);
    {
        std::lock_guard<std::mutex> lock(m_clockMutex);
        m_lastSubmittedPtsUs = firstPtsUs + durationUs;
        m_anySubmitted       = true;
    }
    return sampleCount;
}

int64_t AudioPlaybackPulse::getClockUs() const
{
    std::shared_lock<std::shared_mutex> streamLock(m_streamMutex);
    if (!m_stream) return 0;
    std::lock_guard<std::mutex> lock(m_clockMutex);
    if (!m_anySubmitted) return 0;

    int err = 0;
    pa_usec_t latency = pa_simple_get_latency(m_stream, &err);
    if (err != 0)
    {
        latency = 0;
    }
    int64_t clock = m_lastSubmittedPtsUs - static_cast<int64_t>(latency);
    if (clock < 0) clock = 0;
    return clock;
}

void AudioPlaybackPulse::flush()
{
    std::shared_lock<std::shared_mutex> streamLock(m_streamMutex);
    if (!m_stream) return;
    int err = 0;
    pa_simple_flush(m_stream, &err);
    std::lock_guard<std::mutex> lock(m_clockMutex);
    m_lastSubmittedPtsUs = 0;
    m_anySubmitted       = false;
}

#endif // __linux__
