#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

/**
 * In-process fan-out for captured S16LE interleaved audio.
 *
 * One producer (the platform audio capture) calls push(); multiple
 * consumers each hold an independent Tap and pull at their own pace:
 * the encoder/recorder drain (IAudioCapture::getSamples), the local
 * monitor playback and the module-pipe-source publisher.
 *
 * Every push carries the MediaClock capture time of its first sample
 * (#197). Each tap keeps those timestamps anchored to the samples they
 * belong to — through partial pulls and drop-oldest overflow — so a
 * consumer always learns when the first sample it pulled was captured,
 * not when it happened to pull it. That is what lets the encoder keep
 * audio on the same timeline as video regardless of delivery jitter,
 * main-loop stalls or bus backlog (#201).
 *
 * The seam exists so a future DSP chain can sit between push() and the
 * publish taps without rearchitecting capture or consumers.
 */
class AudioBus
{
public:
    class Tap
    {
    public:
        // Pulls up to maxSamples interleaved int16 samples into dst.
        // Returns the count actually copied. Non-blocking.
        size_t pull(int16_t *dst, size_t maxSamples);

        // Same, and reports the MediaClock capture time (µs) of the first
        // sample copied. firstSampleTsUs is left untouched when nothing
        // was pulled.
        size_t pull(int16_t *dst, size_t maxSamples, int64_t &firstSampleTsUs);

        // Discard up to maxSamples from the front (oldest) of the queue.
        size_t discard(size_t maxSamples);

        size_t available() const;

    private:
        friend class AudioBus;

        Tap(size_t capacitySamples, uint32_t sampleRate, uint32_t channels)
            : m_capacity(capacitySamples), m_sampleRate(sampleRate), m_channels(channels) {}

        void push(const int16_t *src, size_t sampleCount, int64_t firstSampleTsUs);
        void setFormat(uint32_t sampleRate, uint32_t channels);

        // Capture time of the sample at absolute index `abs` (must be
        // inside the queue). Caller holds m_mutex.
        int64_t timestampOfLocked(uint64_t abs) const;
        // Pop the front `n` samples and drop anchors no longer needed.
        void popFrontLocked(size_t n);

        struct Anchor
        {
            uint64_t absIndex; // absolute index of the first sample of a push
            int64_t tsUs;      // its capture time
        };

        // Drop-oldest when full so a stalled consumer can never wedge
        // the producer or starve the other taps.
        size_t              m_capacity;
        uint32_t            m_sampleRate;
        uint32_t            m_channels;
        mutable std::mutex  m_mutex;
        std::deque<int16_t> m_buffer;
        std::deque<Anchor>  m_anchors;  // ascending absIndex; front <= m_frontAbs
        uint64_t            m_frontAbs = 0; // absolute index of m_buffer.front()
    };

    AudioBus(uint32_t sampleRate, uint32_t channels);

    uint32_t getSampleRate() const;
    uint32_t getChannels() const;

    // Update the format after the producer renegotiated it (e.g. Core
    // Audio adopting the device's native rate). Propagates to every tap.
    void setFormat(uint32_t sampleRate, uint32_t channels);

    // Caller owns the returned tap; AudioBus only holds a weak ref, so
    // dropping the last shared_ptr cleanly removes the tap on the next
    // push().
    std::shared_ptr<Tap> createTap(size_t capacitySamples);

    // firstSampleTsUs: MediaClock capture time of interleaved[0].
    void push(const int16_t *interleaved, size_t sampleCount, int64_t firstSampleTsUs);

    // Legacy overload for producers that don't know the capture time:
    // assumes the last sample was captured "now" (the push happens right
    // after the device delivered the block).
    void push(const int16_t *interleaved, size_t sampleCount);

private:
    mutable std::mutex                m_formatMutex;
    uint32_t                          m_sampleRate;
    uint32_t                          m_channels;

    mutable std::mutex                m_tapsMutex;
    std::vector<std::weak_ptr<Tap>>   m_taps;
};
