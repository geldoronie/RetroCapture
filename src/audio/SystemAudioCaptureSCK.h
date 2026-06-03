#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

/**
 * macOS system-audio capture via ScreenCaptureKit (macOS 13+), #109.
 *
 * Captures the computer's output audio (a loopback there is none of
 * natively in Core Audio) by running an audio-only SCStream and pushing
 * the PCM into the shared AudioBus, same as the mic path. Isolated from
 * AudioCaptureCoreAudio's AudioUnit code so it can't disturb mic capture.
 *
 * Feedback (microfonia) is avoided two ways: the caller disables the
 * local monitor playback when capturing system audio, and the stream is
 * configured with excludesCurrentProcessAudio so our own output never
 * appears in the captured mix.
 */
class SystemAudioCaptureSCK
{
public:
    SystemAudioCaptureSCK();
    ~SystemAudioCaptureSCK();

    // Interleaved int16 PCM sink, invoked on SCK's audio queue for each
    // captured buffer. Routed through SckSystemAudioHub so it can track
    // recency and run the silence keepalive.
    using SampleSink = std::function<void(const int16_t *, std::size_t)>;

    // Begin capturing system audio, delivering samples to `sink` at the
    // given format. Returns false on an immediate setup failure (buffers
    // still arrive async).
    bool start(SampleSink sink, uint32_t sampleRate, uint32_t channels);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
