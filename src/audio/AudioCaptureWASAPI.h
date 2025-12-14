#pragma once

#include "IAudioCapture.h"
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

    // State
    bool m_isOpen;
    bool m_isCapturing;
    std::string m_deviceId;

    // Audio buffer (thread-safe)
    std::vector<int16_t> m_audioBuffer;
    std::mutex m_bufferMutex;

    // Callbacks
    std::function<void(const int16_t *data, size_t samples)> m_audioCallback;
    std::function<void(const std::string &, bool)> m_deviceStateCallback;

    // Capture thread
    std::atomic<bool> m_captureThreadRunning;
    std::thread m_captureThread;

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
};

#endif // _WIN32

