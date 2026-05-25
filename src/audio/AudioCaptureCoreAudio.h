#pragma once

#include "IAudioCapture.h"
#include "AudioBus.h"
#include "IAudioOutput.h"
#include <atomic>
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <functional>
#include <thread>

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

    // In-process fan-out — mirror of Linux's AudioCapturePulse design.
    // Producer is the Core Audio input callback; consumers (the
    // encoder/recorder via getSamples, plus the host-side monitor) each
    // hold an AudioBus::Tap. Exposed publicly so future consumers (a
    // macOS source publisher, a DSP chain, etc.) can attach to the
    // same bus without rearchitecting capture.
    AudioBus *getBus() const { return m_bus.get(); }

    // Drop any backlog accumulated in the host-side monitor (typically
    // after a stall) so the user hears live audio again. Counterpart
    // to AudioCapturePulse::resyncMonitor on Linux.
    void resyncMonitor();

private:
#ifdef __APPLE__
    AudioComponentInstance m_audioUnit;
    AudioComponent m_audioComponent;
    AudioCaptureContext* m_context;
    std::mutex m_bufferMutex;
#endif

    uint32_t m_sampleRate;
    uint32_t m_channels;
    uint32_t m_bytesPerSample;
    bool m_isOpen;
    bool m_isCapturing;
    std::function<void(const std::string &, bool)> m_deviceStateCallback;

    // In-process fan-out + the local tap that backs getSamples().
    std::unique_ptr<AudioBus>      m_bus;
    std::shared_ptr<AudioBus::Tap> m_localTap;

    // Host-side monitor playback (Core Audio counterpart of Linux's
    // MonitorPlayback). Owned here so the macOS audio side mirrors the
    // "AudioCapturePulse owns its monitor" pattern from #78.
    std::unique_ptr<IAudioOutput>  m_monitor;
    std::shared_ptr<AudioBus::Tap> m_monitorTap;
    std::atomic<bool>              m_monitorRunning{false};
    std::atomic<bool>              m_monitorResyncPending{false};
    std::thread                    m_monitorThread;

    // Internal methods
    bool initializeAudioUnit();
    void cleanupAudioUnit();
    bool startMonitor();
    void stopMonitor();
    void monitorWriterLoop();
};
