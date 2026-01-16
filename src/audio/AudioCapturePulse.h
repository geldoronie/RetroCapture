#pragma once

#include "IAudioCapture.h"
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <functional>

// Forward declarations for PulseAudio
struct pa_context;
struct pa_stream;
struct pa_mainloop;
struct pa_mainloop_api;
struct pa_sink_info;
struct pa_source_info;
struct pa_sink_input_info;

/**
 * @brief PulseAudio implementation of IAudioCapture for Linux
 */
class AudioCapturePulse : public IAudioCapture
{
public:
    AudioCapturePulse();
    ~AudioCapturePulse() override;

    // Non-copyable
    AudioCapturePulse(const AudioCapturePulse &) = delete;
    AudioCapturePulse &operator=(const AudioCapturePulse &) = delete;

    // IAudioCapture interface
    bool open(const std::string &deviceName = "") override;
    void close() override;
    bool isOpen() const override;
    size_t getSamples(std::vector<float> &samples) override;
    uint32_t getSampleRate() const override;
    uint32_t getChannels() const override;
    std::vector<AudioDeviceInfo> listDevices() override;
    void setDeviceStateCallback(std::function<void(const std::string &, bool)> callback) override;

    // Additional PulseAudio-specific methods (for backward compatibility)
    bool startCapture();
    void stopCapture();
    size_t getSamples(int16_t *buffer, size_t maxSamples);
    uint32_t getBytesPerSample() const { return m_bytesPerSample; }
    std::vector<std::string> getAvailableDevices();
    void setAudioCallback(std::function<void(const int16_t *data, size_t samples)> callback);

    // Audio input management
    // Input: connect audio source directly to RetroCapture:input_FL/FR ports (for capture)
    bool connectInputSource(const std::string &sourceName);
    void disconnectInputSource();
    std::string getCurrentInputSource() const { return m_currentInputSourceName; }
    
    // List available devices
    std::vector<AudioDeviceInfo> listInputSources(); // Audio sources (inputs)

private:
    // PulseAudio callbacks
    static void contextStateCallback(pa_context *c, void *userdata);
    static void streamStateCallback(pa_stream *s, void *userdata);
    static void streamReadCallback(pa_stream *s, size_t length, void *userdata);
    static void streamSuccessCallback(pa_stream *s, int success, void *userdata);
    static void sinkInfoCallback(pa_context *c, const pa_sink_info *i, int eol, void *userdata);
    static void sourceInfoCallback(pa_context *c, const pa_source_info *i, int eol, void *userdata);
    static void operationCallback(pa_context *c, uint32_t index, void *userdata);

    // Internal methods
    void contextStateChanged();
    void streamStateChanged();
    void streamRead(size_t length);
    bool initializePulseAudio();
    void cleanupPulseAudio();
    bool createVirtualSink();
    void removeVirtualSink();
    void waitForContextReady();
    void cleanupOrphanedLoopbacks();
    void cleanupOrphanedSinkInputs();

    // PulseAudio objects
    pa_mainloop *m_mainloop;
    pa_mainloop_api *m_mainloopApi;
    pa_context *m_context;
    pa_stream *m_stream;
    uint32_t m_virtualSinkIndex;      // RetroCapture sink index
    uint32_t m_moduleIndex;            // Module index for RetroCapture sink
    uint32_t m_inputLoopbackModuleIndex;  // Module index for input source connection

    // Audio format
    uint32_t m_sampleRate;
    uint32_t m_channels;
    uint32_t m_bytesPerSample;
    std::string m_deviceName;

    // State
    bool m_isOpen;
    bool m_isCapturing;

    // Audio buffer (thread-safe)
    std::vector<int16_t> m_audioBuffer;
    std::mutex m_bufferMutex;
    
    // Mainloop mutex (thread-safe access to pa_mainloop)
    std::mutex m_mainloopMutex;

    // Callbacks
    std::function<void(const int16_t *data, size_t samples)> m_audioCallback;
    std::function<void(const std::string &, bool)> m_deviceStateCallback;

    // Input management
    std::string m_currentInputSourceName;      // Currently connected input source
};
