#pragma once

#include "IAudioCapture.h"
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <functional>

#ifdef __APPLE__
#import <CoreAudio/CoreAudio.h>
#import <AudioUnit/AudioUnit.h>
#import <CoreFoundation/CoreFoundation.h>

// Forward declaration da estrutura de contexto
struct AudioCaptureContext;
#endif

/**
 * @brief Core Audio implementation of IAudioCapture for macOS
 */
class AudioCaptureCoreAudio : public IAudioCapture
{
public:
    AudioCaptureCoreAudio();
    ~AudioCaptureCoreAudio() override;

    // Non-copyable
    AudioCaptureCoreAudio(const AudioCaptureCoreAudio &) = delete;
    AudioCaptureCoreAudio &operator=(const AudioCaptureCoreAudio &) = delete;

    // IAudioCapture interface
    bool open(const std::string &deviceName = "") override;
    void close() override;
    bool isOpen() const override;
    size_t getSamples(std::vector<float> &samples) override;
    uint32_t getSampleRate() const override;
    uint32_t getChannels() const override;
    std::vector<AudioDeviceInfo> listDevices() override;
    void setDeviceStateCallback(std::function<void(const std::string &, bool)> callback) override;

    // Additional Core Audio-specific methods (for backward compatibility)
    bool startCapture() override;
    void stopCapture() override;
    size_t getSamples(int16_t *buffer, size_t maxSamples) override;
    uint32_t getBytesPerSample() const override { return m_bytesPerSample; }

    // Método público para callback acessar AudioUnit
#ifdef __APPLE__
    AudioComponentInstance getAudioUnit() const;
#endif

private:
#ifdef __APPLE__
    AudioComponentInstance m_audioUnit;
    AudioComponent m_audioComponent;
    AudioCaptureContext* m_context; // Ponteiro tipado
    std::mutex m_bufferMutex;
    std::vector<int16_t> m_audioBuffer;
#endif

    uint32_t m_sampleRate;
    uint32_t m_channels;
    uint32_t m_bytesPerSample;
    bool m_isOpen;
    bool m_isCapturing;
    std::function<void(const std::string &, bool)> m_deviceStateCallback;

    // Internal methods
    bool initializeAudioUnit();
    void cleanupAudioUnit();
};
