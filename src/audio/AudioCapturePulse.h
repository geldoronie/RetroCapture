#pragma once

#include "IAudioCapture.h"
#include "AudioBus.h"
#include "PipeSourcePublisher.h"
#include "MonitorPlayback.h"
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
struct pa_source_info;

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
    // Switch to a different PulseAudio source as the input device. Tears
    // down the existing record stream and reconnects against the new
    // source. Empty string means "PulseAudio default device".
    bool connectInputSource(const std::string &sourceName);
    // Detach the input device. Closes the record stream but keeps the PA
    // context alive so a subsequent connectInputSource() reattaches
    // immediately.
    void disconnectInputSource();
    std::string getCurrentInputSource() const { return m_currentInputSourceName; }
    
    // List available devices
    std::vector<AudioDeviceInfo> listInputSources(); // Audio sources (inputs)

    // In-process fan-out bus. Owned by this capture; other consumers
    // (e.g. the module-pipe-source publisher exposing the `RetroCapture`
    // virtual source) attach a Tap and pull at their own pace. Lives as
    // long as the capture is open.
    AudioBus *getBus() const { return m_bus.get(); }

    // Drop any backlog accumulated in the monitor playback (typically
    // after a stall) so the user hears live audio again instead of the
    // delayed buffer that built up during the stall. Safe to call from
    // any thread; no-op if monitor is not running.
    void resyncMonitor();

private:
    // PulseAudio callbacks
    static void contextStateCallback(pa_context *c, void *userdata);
    static void streamStateCallback(pa_stream *s, void *userdata);
    static void streamReadCallback(pa_stream *s, size_t length, void *userdata);
    static void streamSuccessCallback(pa_stream *s, int success, void *userdata);
    static void sourceInfoCallback(pa_context *c, const pa_source_info *i, int eol, void *userdata);
    static void operationCallback(pa_context *c, uint32_t index, void *userdata);

    // Internal methods
    void contextStateChanged();
    void streamStateChanged();
    void streamRead(size_t length);
    bool initializePulseAudio();
    void cleanupPulseAudio();
    // Build + connect a PA_STREAM_RECORD stream against `device` (empty
    // == PulseAudio default). Replaces any existing m_stream.
    bool connectRecordStream(const std::string &device);
    // Disconnect and free m_stream without touching the PA context.
    void disconnectRecordStream();
    // Create the FIFO, load module-pipe-source with source_name=
    // `RetroCapture`, and spawn the writer thread that drains the bus
    // into the FIFO. Together these publish the virtual source visible
    // to the rest of the OS audio graph.
    bool startPublishSource();
    void stopPublishSource();
    void waitForContextReady();
    // One-shot migration GC: unloads any module-null-sink (sink_name=
    // RetroCapture) and module-loopback (feeding sink=RetroCapture)
    // left behind by pre-0.8 RetroCapture binaries on this host. No-op
    // on a clean graph. Safe to drop in a future release once the
    // 0.7→0.8 transition window has closed.
    void gcLegacyRetroCaptureModules();

    // PulseAudio objects
    pa_mainloop *m_mainloop;
    pa_mainloop_api *m_mainloopApi;
    pa_context *m_context;
    pa_stream *m_stream;

    // Audio format
    uint32_t m_sampleRate;
    uint32_t m_channels;
    uint32_t m_bytesPerSample;
    std::string m_deviceName;

    // State
    bool m_isOpen;
    bool m_isCapturing;

    // In-process fan-out + the local tap that backs getSamples().
    // The old m_audioBuffer is now this tap's queue; the producer side
    // (streamRead) pushes into m_bus, every live tap (including this
    // one) receives a copy, and getSamples() drains m_localTap.
    std::unique_ptr<AudioBus>      m_bus;
    std::shared_ptr<AudioBus::Tap> m_localTap;

    // module-pipe-source publisher: FIFO + writer thread that exposes
    // the `RetroCapture` virtual source to the OS audio graph (the
    // "input" half of the RetroCapture I/O pair).
    std::unique_ptr<PipeSourcePublisher> m_publisher;
    uint32_t                              m_pipeSourceModuleIndex = 0; // PA_INVALID_INDEX sentinel set in ctor
    std::string                           m_fifoPath;
    std::string                           m_fifoDir;

    // Local monitor playback (the "output" half of the RetroCapture
    // I/O pair) — drains a tap into PulseAudio's default sink so the
    // user hears the captured audio without any pavucontrol routing.
    // Mirror of the client's AudioPlaybackPulse semantics. Owned by us
    // (not delegated to module-loopback) so a future DSP chain can
    // sit between the bus and the playback.
    std::unique_ptr<MonitorPlayback> m_monitor;
    
    // Mainloop mutex (thread-safe access to pa_mainloop)
    std::mutex m_mainloopMutex;

    // Callbacks
    std::function<void(const int16_t *data, size_t samples)> m_audioCallback;
    std::function<void(const std::string &, bool)> m_deviceStateCallback;

    // Input management
    std::string m_currentInputSourceName;      // Currently connected input source
};
