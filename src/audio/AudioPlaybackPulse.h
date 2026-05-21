#pragma once

#include "IAudioPlayback.h"

#ifdef __linux__

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>

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
    bool isOpen() const override
    {
        // Synchronizes with the unique-locked open/close so callers
        // can't observe a half-torn-down m_stream.
        std::shared_lock<std::shared_mutex> lock(m_streamMutex);
        return m_stream != nullptr;
    }

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

    // Guards m_stream lifetime against concurrent pa_simple_* calls.
    // The video render thread queries getClockUs() while the decode
    // thread can be inside submit()/flush() — and on a TLS hiccup the
    // same decode thread tears down via close(). Without serializing,
    // close() invokes pa_simple_free while another thread is mid-call,
    // and PulseAudio's pa_threaded_mainloop aborts on the still-held
    // internal mutex.
    //
    // open()/close()  → exclusive (writers)
    // submit()/flush()/getClockUs()/isOpen() → shared (readers)
    //
    // pa_simple_* is internally serialized by the mainloop lock, so
    // concurrent shared holders won't race inside libpulse-simple.
    mutable std::shared_mutex m_streamMutex;
};

#endif // __linux__
