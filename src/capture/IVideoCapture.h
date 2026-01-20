#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <cstddef> // for size_t

struct Frame
{
    uint8_t *data = nullptr;
    size_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0; // Platform-specific pixel format
};

struct DeviceInfo
{
    std::string id;        // Device identifier (path, GUID, etc.)
    std::string name;      // Human-readable name
    std::string driver;    // Driver name (optional)
    bool available = true; // Whether device is available
};

// Format information for AVFoundation devices (macOS)
struct AVFoundationFormatInfo
{
    std::string id;              // Unique identifier for this format (e.g., internal representation)
    uint32_t width = 0;          // Resolution width
    uint32_t height = 0;         // Resolution height
    float minFps = 0.0f;         // Minimum supported framerate
    float maxFps = 0.0f;         // Maximum supported framerate
    std::string pixelFormat;     // Pixel format name (e.g., "NV12 (420v)", "YUY2 (yuvs)", "BGRA")
    std::string colorSpace;      // Color space (e.g., "CS 709", "CS 601")
    std::string displayName;     // Human-readable format string (e.g., "1280x720 (16:9) - 10-60 FPS - CS 709 - NV12 (420v)")
};

/**
 * @brief Abstract interface for video capture across different platforms
 */
class IVideoCapture
{
public:
    virtual ~IVideoCapture() = default;

    virtual bool open(const std::string &device) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual bool setFormat(uint32_t width, uint32_t height, uint32_t pixelFormat = 0) = 0;
    virtual bool setFramerate(uint32_t fps) = 0;
    virtual bool captureFrame(Frame &frame) = 0;
    virtual bool setControl(const std::string &controlName, int32_t value) = 0;
    virtual bool getControl(const std::string &controlName, int32_t &value) = 0;
    virtual bool getControlMin(const std::string &controlName, int32_t &minValue) = 0;
    virtual bool getControlMax(const std::string &controlName, int32_t &maxValue) = 0;
    virtual bool getControlDefault(const std::string &controlName, int32_t &defaultValue) = 0;
    virtual std::vector<DeviceInfo> listDevices() = 0;
    virtual void setDummyMode(bool enabled) = 0;
    virtual bool isDummyMode() const = 0;

    // Additional methods for backward compatibility
    virtual bool startCapture() = 0;
    virtual void stopCapture() = 0;
    virtual bool captureLatestFrame(Frame &frame) = 0;
    virtual uint32_t getWidth() const = 0;
    virtual uint32_t getHeight() const = 0;
    virtual uint32_t getPixelFormat() const = 0;
    
    // Format enumeration (optional - only AVFoundation implements this)
    virtual std::vector<AVFoundationFormatInfo> listFormats(const std::string &deviceId = "") { (void)deviceId; return {}; }
    virtual bool setFormatById(const std::string &formatId, const std::string &deviceId = "") { (void)formatId; (void)deviceId; return false; }
    
    // Audio capture methods (optional - only AVFoundation implements this)
    virtual bool hasAudio() const { return false; }
    virtual size_t getAudioSamples(int16_t* buffer, size_t maxSamples) { (void)buffer; (void)maxSamples; return 0; }
    virtual uint32_t getAudioSampleRate() const { return 0; }
    virtual uint32_t getAudioChannels() const { return 0; }
    
    // Audio device enumeration and selection (optional - only AVFoundation implements this)
    virtual std::vector<DeviceInfo> listAudioDevices() { return {}; }
    virtual bool setAudioDevice(const std::string &audioDeviceId) { (void)audioDeviceId; return false; }
    virtual std::string getCurrentAudioDevice() const { return ""; }
};

