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
 * One producer (the platform audio capture, currently AudioCapturePulse)
 * calls push(); multiple consumers each hold an independent Tap and pull
 * at their own pace. Day-one consumers are the encoder/recorder drain
 * (existing IAudioCapture::getSamples path) and the future
 * module-pipe-source publisher that exposes the `RetroCapture` virtual
 * source to the rest of the OS audio graph.
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
        // Pulls up to maxSamples interleaved int16 frames into dst.
        // Returns the count actually copied. Non-blocking.
        size_t pull(int16_t *dst, size_t maxSamples);

        size_t available() const;

    private:
        friend class AudioBus;

        explicit Tap(size_t capacitySamples) : m_capacity(capacitySamples) {}

        void push(const int16_t *src, size_t sampleCount);

        // Drop-oldest when full so a stalled consumer can never wedge
        // the producer or starve the other taps.
        size_t              m_capacity;
        mutable std::mutex  m_mutex;
        std::deque<int16_t> m_buffer;
    };

    AudioBus(uint32_t sampleRate, uint32_t channels);

    uint32_t getSampleRate() const { return m_sampleRate; }
    uint32_t getChannels() const { return m_channels; }

    // Caller owns the returned tap; AudioBus only holds a weak ref, so
    // dropping the last shared_ptr cleanly removes the tap on the next
    // push().
    std::shared_ptr<Tap> createTap(size_t capacitySamples);

    void push(const int16_t *interleaved, size_t sampleCount);

private:
    uint32_t m_sampleRate;
    uint32_t m_channels;

    mutable std::mutex                m_tapsMutex;
    std::vector<std::weak_ptr<Tap>>   m_taps;
};
