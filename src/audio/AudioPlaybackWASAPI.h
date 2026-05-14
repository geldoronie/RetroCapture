#pragma once

#include "IAudioPlayback.h"

#ifdef _WIN32

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

struct IMMDevice;
struct IMMDeviceEnumerator;
struct IAudioClient;
struct IAudioRenderClient;
struct IAudioClock;

/**
 * Windows / WASAPI render sink. We open a shared-mode rendering
 * stream against the default playback endpoint with the format the
 * caller asks for (sample rate / channel count of the incoming /raw
 * audio); WASAPI handles resampling to the device mix format
 * automatically in shared mode.
 *
 * A background thread services the WASAPI event handle and drains
 * our intermediate float-PCM queue into the IAudioRenderClient. The
 * decode-side submit() pushes onto the queue without blocking —
 * critical because the video decode loop sits in av_read_frame the
 * rest of the time and can't afford an audio-side stall.
 *
 * getClockUs() reads IAudioClock::GetPosition() to find out how many
 * device frames have actually played, then subtracts that from the
 * total submitted to derive the timestamp currently emerging from
 * the speakers (the master A/V clock).
 */
class AudioPlaybackWASAPI : public IAudioPlayback
{
public:
    AudioPlaybackWASAPI();
    ~AudioPlaybackWASAPI() override;

    bool open(uint32_t sampleRate, uint32_t channels) override;
    void close() override;
    bool isOpen() const override { return m_audioClient != nullptr; }

    size_t submit(const float *interleaved,
                  size_t sampleCount,
                  int64_t firstPtsUs) override;

    int64_t getClockUs() const override;
    void    flush() override;

private:
    void renderThreadFn();

    IMMDeviceEnumerator *m_enumerator   = nullptr;
    IMMDevice           *m_device       = nullptr;
    IAudioClient        *m_audioClient  = nullptr;
    IAudioRenderClient  *m_renderClient = nullptr;
    IAudioClock         *m_audioClock   = nullptr;
    void                *m_eventHandle  = nullptr; // HANDLE

    uint32_t m_sampleRate = 0;
    uint32_t m_channels   = 0;
    uint32_t m_bufferFrameCount = 0;

    // Producer/consumer queue between submit() and the render thread.
    // Float-interleaved; lengths in *frames* (not floats).
    mutable std::mutex   m_queueMutex;
    std::vector<float>   m_queue;            // big circular-ish vector, drained from the front
    std::atomic<bool>    m_running{false};
    std::thread          m_renderThread;

    // Clock tracking: 'submitted frames' counts everything we've
    // queued; 'firstQueuedPtsUs' is the PTS of the first frame still
    // in the queue. Combined with IAudioClock's played-frames count
    // they let us compute the PTS currently at the speakers.
    mutable std::mutex m_clockMutex;
    uint64_t           m_submittedFrames    = 0;
    int64_t            m_firstSubmittedPtsUs = 0;
    bool               m_anySubmitted       = false;
};

#endif // _WIN32
