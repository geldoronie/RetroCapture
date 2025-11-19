#pragma once

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

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    // Non-copyable
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // Open audio capture device
    bool open(const std::string& deviceName = "");
    
    // Close audio capture
    void close();
    
    // Check if audio capture is open
    bool isOpen() const { return m_isOpen; }
    
    // Start capturing audio
    bool startCapture();
    
    // Stop capturing audio
    void stopCapture();
    
    // Get latest audio samples
    // Returns number of samples read
    size_t getSamples(int16_t* buffer, size_t maxSamples);
    
    // Get audio format information
    uint32_t getSampleRate() const { return m_sampleRate; }
    uint32_t getChannels() const { return m_channels; }
    uint32_t getBytesPerSample() const { return m_bytesPerSample; }
    
    // Get available audio devices
    std::vector<std::string> getAvailableDevices();
    
    // Set callback for audio data
    void setAudioCallback(std::function<void(const int16_t* data, size_t samples)> callback);

private:
    // PulseAudio callbacks
    static void contextStateCallback(pa_context* c, void* userdata);
    static void streamStateCallback(pa_stream* s, void* userdata);
    static void streamReadCallback(pa_stream* s, size_t length, void* userdata);
    static void streamSuccessCallback(pa_stream* s, int success, void* userdata);
    
    // Internal methods
    void contextStateChanged();
    void streamStateChanged();
    void streamRead(size_t length);
    bool initializePulseAudio();
    void cleanupPulseAudio();
    bool createVirtualSink();
    void removeVirtualSink();
    
    // PulseAudio callbacks for sink operations
    static void sinkInfoCallback(pa_context* c, const pa_sink_info* i, int eol, void* userdata);
    static void operationCallback(pa_context* c, uint32_t index, void* userdata);
    
    // PulseAudio objects
    pa_mainloop* m_mainloop;
    pa_mainloop_api* m_mainloopApi;
    pa_context* m_context;
    pa_stream* m_stream;
    uint32_t m_virtualSinkIndex; // Index do sink virtual criado
    uint32_t m_moduleIndex; // Index do módulo PulseAudio carregado (para remoção)
    
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
    
    // Callback
    std::function<void(const int16_t* data, size_t samples)> m_audioCallback;
};

