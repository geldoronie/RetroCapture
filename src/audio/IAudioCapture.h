#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <functional>

struct AudioDeviceInfo
{
    std::string id;          // Device identifier
    std::string name;        // Human-readable name
    std::string description; // Device description (optional)
    bool available = true;   // Whether device is available
    // #109 — true when this entry is an output device's monitor/loopback
    // (system audio) rather than a real input. The UI groups these
    // separately and the capture skips local monitor playback for them
    // to avoid a feedback loop.
    bool isMonitor = false;
};

/**
 * @brief Abstract interface for audio capture across different platforms
 */
class IAudioCapture
{
public:
    virtual ~IAudioCapture() = default;

    virtual bool open(const std::string &deviceName = "") = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual size_t getSamples(std::vector<float> &samples) = 0;
    virtual uint32_t getSampleRate() const = 0;
    virtual uint32_t getChannels() const = 0;
    virtual std::vector<AudioDeviceInfo> listDevices() = 0;
    virtual void setDeviceStateCallback(std::function<void(const std::string &, bool)> callback) = 0;

    // Additional methods for backward compatibility
    virtual bool startCapture() = 0;
    virtual void stopCapture() = 0;
    virtual size_t getSamples(int16_t *buffer, size_t maxSamples) = 0;
    virtual uint32_t getBytesPerSample() const = 0;
};

