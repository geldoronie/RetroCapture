#pragma once

#include "IAudioPlayback.h"

#ifdef __linux__

#include <atomic>
#include <cstdint>
#include <mutex>

struct pa_simple;

/**
 * PulseAudio playback sink implemented via the synchronous pa_simple
 * API. Each submit() writes directly to the daemon; the daemon
 * buffers ~the configured tlength worth of audio (~50 ms here) and
 * blocks the writer if the buffer fills, which gives us natural
 * back-pressure without a dedicated playback thread.
 *
 * The video path queries getClockUs() to discover what timestamp the
 * speakers are currently emitting, which becomes the A/V master clock.
 * We track the PTS of the most recently submitted sample and subtract
 * pa_simple_get_latency() to get the playing-now timestamp.
 */
class AudioPlaybackPulse : public IAudioPlayback
{
public:
    AudioPlaybackPulse();
    ~AudioPlaybackPulse() override;

    bool open(uint32_t sampleRate, uint32_t channels) override;
    void close() override;
    bool isOpen() const override { return m_stream != nullptr; }

    size_t submit(const float *interleaved,
                  size_t sampleCount,
                  int64_t firstPtsUs) override;

    int64_t getClockUs() const override;
    void    flush() override;

private:
    pa_simple        *m_stream      = nullptr;
    uint32_t          m_sampleRate  = 0;
    uint32_t          m_channels    = 0;
    // Last PTS we handed to pa_simple_write, in stream-origin-relative
    // microseconds. Updated under m_clockMutex so the video thread can
    // sample it consistently.
    mutable std::mutex m_clockMutex;
    int64_t            m_lastSubmittedPtsUs = 0;
    bool               m_anySubmitted       = false;
};

#endif // __linux__
