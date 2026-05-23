#pragma once

#include "AudioBus.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

struct pa_simple;

/**
 * Local monitor playback: pulls samples from an AudioBus tap and writes
 * them to PulseAudio's default sink under the stream name `RetroCapture`.
 *
 * Mirror of the client's AudioPlaybackPulse — both use pa_simple to
 * own the playback path in-process rather than delegate to a Pulse
 * loopback module. Satisfies the "parity with client" requirement of
 * issue #78 (the host-side symmetric output to the client's playback
 * stream named `RetroCapture`) AND keeps the audio under our control
 * so a future DSP chain can sit between the bus and the playback.
 *
 * The capture device's clock and the playback sink's clock are
 * independent hardware crystals; even nominally-44100 Hz they differ
 * by ppm and would drift visibly over a long session if we just
 * blindly pa_simple_write everything the tap delivers. The writer
 * loop watches tap backlog and discards excess samples once it crosses
 * a small catch-up threshold — that bounds monitor latency to a small
 * constant regardless of how the two clocks differ.
 */
class MonitorPlayback
{
public:
    MonitorPlayback() = default;
    ~MonitorPlayback();

    MonitorPlayback(const MonitorPlayback &)            = delete;
    MonitorPlayback &operator=(const MonitorPlayback &) = delete;

    bool start(std::shared_ptr<AudioBus::Tap> tap,
               uint32_t sampleRate, uint32_t channels);
    void stop();
    bool isRunning() const { return m_running.load(); }

    // Snap monitor latency back to ~tlength (50 ms) by draining the
    // tap and flushing PulseAudio's playback buffer at the next
    // writer-thread iteration. Useful after a stall that left the
    // monitor playing behind real-time. Cheap atomic flag — no
    // cross-thread pa_simple call, no lock contention on the hot path.
    void requestResync() { m_resyncPending = true; }

private:
    void writerLoop();

    pa_simple                     *m_stream     = nullptr;
    uint32_t                       m_sampleRate = 0;
    uint32_t                       m_channels   = 0;
    std::shared_ptr<AudioBus::Tap> m_tap;
    std::atomic<bool>              m_running{false};
    std::atomic<bool>              m_resyncPending{false};
    std::thread                    m_thread;
};
