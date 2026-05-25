#pragma once

#include "IAudioPlayback.h"

#ifdef __APPLE__

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

class IAudioOutput;

/**
 * Core Audio playback sink for the remote-stream audio path
 * (macOS counterpart of AudioPlaybackPulse / AudioPlaybackWASAPI).
 *
 * Wraps AudioOutputCoreAudio (which already drives a HAL Output
 * AudioUnit) and adapts its int16 / push API to IAudioPlayback's
 * float-PCM + PTS API. The PTS bookkeeping mirrors AudioPlaybackPulse:
 * we remember the PTS of the last sample handed to write() and
 * subtract the hardware-side buffered samples worth of time to give
 * the video render thread an "audio-now" timestamp via getClockUs().
 */
class AudioPlaybackCoreAudio : public IAudioPlayback
{
public:
    AudioPlaybackCoreAudio();
    ~AudioPlaybackCoreAudio() override;

    bool   open(uint32_t sampleRate, uint32_t channels) override;
    void   close() override;
    bool   isOpen() const override;
    size_t submit(const float *interleaved,
                  size_t sampleCount,
                  int64_t firstPtsUs) override;
    int64_t getClockUs() const override;
    void    flush() override;

private:
    std::unique_ptr<IAudioOutput> m_output;
    uint32_t m_sampleRate = 0;
    uint32_t m_channels   = 0;

    // PTS of the most recent sample handed to write(), in stream-
    // origin-relative microseconds. Updated under m_clockMutex.
    mutable std::mutex m_clockMutex;
    int64_t            m_lastSubmittedPtsUs = 0;
    // Samples currently in the IAudioOutput's internal buffer (best-
    // effort; we update this by counting submitted - estimated-drained).
    std::atomic<int64_t> m_bufferedSamples{0};
    bool               m_anySubmitted = false;
};

#endif // __APPLE__
