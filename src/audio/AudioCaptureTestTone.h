#pragma once

#include "IAudioCapture.h"
#include "AudioBus.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

/**
 * Synthetic audio capture for the A/V sync test (#196).
 *
 * Behaves like a real capture device: its own thread produces S16LE
 * interleaved stereo at the nominal sample rate, pushed into an AudioBus
 * in ~5 ms fragments. The signal is a 1 kHz beep during the first 100 ms
 * of every whole MediaClock second — the same instants
 * VideoCaptureTestPattern flashes — so the output's A/V offset can be
 * measured from the beep/flash onsets.
 *
 * skewPpm simulates a capture device whose audio crystal runs fast
 * (positive) or slow (negative) against the system clock: the device
 * delivers sampleRate * (1 + skew) samples per real second while
 * claiming sampleRate. That is the mechanism behind progressive drift on
 * real USB capture cards (#124); the beeps stay locked to real time, so a
 * pipeline that trusts the sample count drifts by exactly the skew.
 *
 * Selected with `--audio-source test`. Never starts a local monitor
 * (a smoke-test run must not beep through the speakers).
 */
class AudioCaptureTestTone : public IAudioCapture
{
public:
    // fragmentMs: delivery granularity, like a real backend's fragment
    // size (the current PulseAudio capture uses 100 ms).
    explicit AudioCaptureTestTone(int skewPpm = 0, int fragmentMs = 10);
    ~AudioCaptureTestTone() override;

    AudioCaptureTestTone(const AudioCaptureTestTone &) = delete;
    AudioCaptureTestTone &operator=(const AudioCaptureTestTone &) = delete;

    bool open(const std::string &deviceName = "") override;
    void close() override;
    bool isOpen() const override { return m_open; }
    size_t getSamples(std::vector<float> &samples) override;
    uint32_t getSampleRate() const override { return m_sampleRate; }
    uint32_t getChannels() const override { return m_channels; }
    std::vector<AudioDeviceInfo> listDevices() override;
    void setDeviceStateCallback(std::function<void(const std::string &, bool)>) override {}

    bool startCapture() override { return m_open; }
    void stopCapture() override {}
    size_t getSamples(int16_t *buffer, size_t maxSamples) override;
    uint32_t getBytesPerSample() const override { return 2; }

private:
    void generatorLoop();

    const uint32_t m_sampleRate = 48000;
    const uint32_t m_channels = 2;
    const int m_skewPpm;
    const int m_fragmentMs;

    std::unique_ptr<AudioBus> m_bus;
    std::shared_ptr<AudioBus::Tap> m_localTap;

    std::atomic<bool> m_open{false};
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};
