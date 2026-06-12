#pragma once

#include "IAudioCapture.h"
#include "AudioPlaybackWASAPI.h"
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <functional>
#include <atomic>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <comdef.h>

// Forward declarations
struct IMMDeviceEnumerator;
struct IMMDevice;
struct IAudioClient;
struct IAudioCaptureClient;
struct IAudioEndpointVolume;

/**
 * @brief WASAPI implementation of IAudioCapture for Windows
 */
class AudioCaptureWASAPI : public IAudioCapture
{
public:
    AudioCaptureWASAPI();
    ~AudioCaptureWASAPI() override;

    // Non-copyable
    AudioCaptureWASAPI(const AudioCaptureWASAPI &) = delete;
    AudioCaptureWASAPI &operator=(const AudioCaptureWASAPI &) = delete;

    // IAudioCapture interface
    bool open(const std::string &deviceName = "") override;
    void close() override;
    bool isOpen() const override;
    size_t getSamples(std::vector<float> &samples) override;
    uint32_t getSampleRate() const override;
    uint32_t getChannels() const override;
    std::vector<AudioDeviceInfo> listDevices() override;
    void setDeviceStateCallback(std::function<void(const std::string &, bool)> callback) override;

    // Additional methods for backward compatibility
    bool startCapture() override;
    void stopCapture() override;
    size_t getSamples(int16_t *buffer, size_t maxSamples) override;
    uint32_t getBytesPerSample() const override;

private:
    // WASAPI objects
    IMMDeviceEnumerator *m_deviceEnumerator;
    IMMDevice *m_device;
    IAudioClient *m_audioClient;
    IAudioCaptureClient *m_captureClient;
    IAudioEndpointVolume *m_endpointVolume;

    // Audio format
    uint32_t m_sampleRate;
    uint32_t m_channels;
    uint32_t m_bytesPerSample;
    WAVEFORMATEX *m_waveFormat;

    // #137 — WASAPI shared-mode mix format is almost always 32-bit IEEE
    // float, not int16. The capture loop used to reinterpret the buffer as
    // int16 unconditionally, which turned the card's audio into white noise.
    // Track the real sample format so processAudioData converts correctly.
    enum class SampleFormat { Pcm16, Pcm32, Float32, Unsupported };
    SampleFormat m_sampleFormat = SampleFormat::Pcm16;

    // State
    bool m_isOpen;
    bool m_isCapturing;
    std::string m_deviceId;
    bool m_useLoopback; // Flag para usar loopback (áudio do sistema) em vez de captura (microfone)

    // Audio buffer (thread-safe)
    std::vector<int16_t> m_audioBuffer;
    std::mutex m_bufferMutex;

    // Callbacks
    std::function<void(const int16_t *data, size_t samples)> m_audioCallback;
    std::function<void(const std::string &, bool)> m_deviceStateCallback;

    // Capture thread
    std::atomic<bool> m_captureThreadRunning;
    std::thread m_captureThread;

    // #137 — local monitor: play the captured audio out the default render
    // endpoint so the operator hears the capture card live (mirrors the
    // PulseAudio/CoreAudio monitor on the other platforms). Disabled for
    // loopback/system-audio sources, which would feed back. Reuses the
    // existing WASAPI render sink.
    std::unique_ptr<AudioPlaybackWASAPI> m_monitor;
    bool m_monitorActive = false;
    std::vector<float> m_monitorScratch;

    // Helper methods
    bool initializeCOM();
    void shutdownCOM();
    bool createDeviceEnumerator();
    bool selectDevice(const std::string &deviceName);
    bool initializeAudioClient();
    bool startCaptureThread();
    void stopCaptureThread();
    void captureThreadFunction();
    void processAudioData(BYTE *data, UINT32 framesAvailable);
    void convertToFloat(const int16_t *input, float *output, size_t samples);
    // #137 — decode the WASAPI mix-format buffer to interleaved float per the
    // detected m_sampleFormat. Returns the number of float samples written.
    size_t decodeToFloat(const BYTE *data, UINT32 framesAvailable, std::vector<float> &out);
};

#endif // _WIN32
