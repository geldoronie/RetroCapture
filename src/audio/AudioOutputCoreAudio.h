#pragma once

#include "IAudioOutput.h"
#include <cstdint>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>

#ifdef __APPLE__
#import <CoreAudio/CoreAudio.h>
#import <AudioUnit/AudioUnit.h>
#import <CoreFoundation/CoreFoundation.h>

// Forward declaration
struct AudioOutputContext;
#endif

/**
 * @brief Core Audio implementation of IAudioOutput for macOS
 */
class AudioOutputCoreAudio : public IAudioOutput
{
public:
    AudioOutputCoreAudio();
    ~AudioOutputCoreAudio() override;

    // Non-copyable
    AudioOutputCoreAudio(const AudioOutputCoreAudio&) = delete;
    AudioOutputCoreAudio& operator=(const AudioOutputCoreAudio&) = delete;

    // IAudioOutput interface
    bool open(const std::string& deviceName = "", uint32_t sampleRate = 0, uint32_t channels = 0) override;
    void close() override;
    bool isOpen() const override;
    bool start() override;
    void stop() override;
    size_t write(const int16_t* samples, size_t numSamples) override;
    uint32_t getSampleRate() const override;
    uint32_t getChannels() const override;
    void setVolume(float volume) override;
    float getVolume() const override;
    bool isEnabled() const override;
    void setEnabled(bool enabled) override;

private:
#ifdef __APPLE__
    AudioComponentInstance m_audioUnit;
    AudioComponent m_audioComponent;
    AudioOutputContext* m_context;
    std::mutex m_bufferMutex;
    std::vector<int16_t> m_audioBuffer;
    std::atomic<float> m_volume;
    std::atomic<bool> m_enabled;
#endif

    uint32_t m_sampleRate;
    uint32_t m_channels;
    bool m_isOpen;
    bool m_isRunning;

    // Internal methods
    bool initializeAudioUnit();
    void cleanupAudioUnit();
};
