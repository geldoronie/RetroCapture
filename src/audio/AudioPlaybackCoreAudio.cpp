#include "AudioPlaybackCoreAudio.h"

#ifdef __APPLE__

#include "IAudioOutput.h"
#include "../utils/Logger.h"

#include <algorithm>
#include <vector>

extern std::unique_ptr<IAudioOutput> createAudioOutputCoreAudio();

AudioPlaybackCoreAudio::AudioPlaybackCoreAudio() = default;

AudioPlaybackCoreAudio::~AudioPlaybackCoreAudio()
{
    close();
}

bool AudioPlaybackCoreAudio::open(uint32_t sampleRate, uint32_t channels)
{
    if (m_output)
    {
        close();
    }
    m_output = createAudioOutputCoreAudio();
    if (!m_output)
    {
        LOG_ERROR("AudioPlaybackCoreAudio: factory returned null");
        return false;
    }
    if (!m_output->open("", sampleRate, channels))
    {
        LOG_ERROR("AudioPlaybackCoreAudio: underlying output open failed");
        m_output.reset();
        return false;
    }
    if (!m_output->start())
    {
        LOG_ERROR("AudioPlaybackCoreAudio: underlying output start failed");
        m_output->close();
        m_output.reset();
        return false;
    }
    m_output->setEnabled(true);

    m_sampleRate = sampleRate;
    m_channels   = channels;
    {
        std::lock_guard<std::mutex> lock(m_clockMutex);
        m_lastSubmittedPtsUs = 0;
        m_anySubmitted       = false;
    }
    m_bufferedSamples = 0;
    LOG_INFO("AudioPlaybackCoreAudio: opened " + std::to_string(sampleRate) +
             " Hz x " + std::to_string(channels) + " ch");
    return true;
}

void AudioPlaybackCoreAudio::close()
{
    if (!m_output)
    {
        return;
    }
    m_output->stop();
    m_output->close();
    m_output.reset();
    m_sampleRate = 0;
    m_channels   = 0;
    m_bufferedSamples = 0;
}

bool AudioPlaybackCoreAudio::isOpen() const
{
    return m_output && m_output->isOpen();
}

size_t AudioPlaybackCoreAudio::submit(const float *interleaved,
                                      size_t sampleCount,
                                      int64_t firstPtsUs)
{
    if (!m_output || !interleaved || sampleCount == 0 || m_channels == 0)
    {
        return 0;
    }

    // Convert interleaved float [-1,1] to interleaved int16. sampleCount
    // is the *frame* count, so total values = sampleCount * channels.
    const size_t totalSamples = sampleCount * m_channels;
    std::vector<int16_t> pcm(totalSamples);
    for (size_t i = 0; i < totalSamples; ++i)
    {
        const float v = std::max(-1.0f, std::min(1.0f, interleaved[i]));
        pcm[i] = static_cast<int16_t>(v * 32767.0f);
    }

    const size_t written = m_output->write(pcm.data(), totalSamples);
    // write() returns # int16 elements written, not frames. Convert back.
    const size_t framesWritten = (m_channels > 0) ? written / m_channels : 0;

    if (framesWritten > 0)
    {
        std::lock_guard<std::mutex> lock(m_clockMutex);
        // Last accepted-frame PTS = firstPtsUs + (framesWritten-1) frame-times.
        const int64_t frameUs = (m_sampleRate > 0)
            ? static_cast<int64_t>(1000000LL) / m_sampleRate
            : 0;
        m_lastSubmittedPtsUs = firstPtsUs +
                               static_cast<int64_t>(framesWritten - 1) * frameUs;
        m_anySubmitted = true;
        m_bufferedSamples.fetch_add(static_cast<int64_t>(framesWritten));
    }
    return framesWritten;
}

int64_t AudioPlaybackCoreAudio::getClockUs() const
{
    std::lock_guard<std::mutex> lock(m_clockMutex);
    if (!m_anySubmitted || m_sampleRate == 0)
    {
        return 0;
    }
    // Subtract the still-buffered frames so the returned timestamp is
    // closer to what the speakers are emitting RIGHT NOW. Without a
    // proper hardware latency query we use the writer-side bookkeeping
    // (samples submitted vs samples we estimate the AudioUnit has
    // already pulled). Loose but usable for A/V sync at < ~50 ms.
    const int64_t bufferedUs = (m_bufferedSamples.load() * 1000000LL) /
                               static_cast<int64_t>(m_sampleRate);
    return m_lastSubmittedPtsUs - bufferedUs;
}

void AudioPlaybackCoreAudio::flush()
{
    // The underlying AudioOutputCoreAudio doesn't currently expose a
    // "drop everything queued" hook; closing + reopening is too heavy
    // for a transient flush. Reset our PTS bookkeeping at least so
    // post-flush samples start the clock fresh.
    std::lock_guard<std::mutex> lock(m_clockMutex);
    m_lastSubmittedPtsUs = 0;
    m_anySubmitted       = false;
    m_bufferedSamples    = 0;
}

#endif // __APPLE__
