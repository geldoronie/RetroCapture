#pragma once

#include <cstdint>
#include <memory>

class AudioBus;

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

    // Begin capturing system audio into `bus` at the given format. The
    // bus outlives this object (owned by AudioCaptureCoreAudio). Returns
    // false on an immediate setup failure (frames still arrive async).
    bool start(AudioBus *bus, uint32_t sampleRate, uint32_t channels);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
