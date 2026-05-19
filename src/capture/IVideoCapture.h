#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

    /**
     * Hint for the UI that a remote capture has been failing to
     * reconnect for long enough that the host is probably down (not
     * just hiccupping). Local capture backends always return false.
     * See VideoCaptureRemote for the threshold (#58).
     */
    virtual bool isHostLikelyOffline() const { return false; }

    /**
     * True iff this capture is actively producing frames right now.
     * Distinct from isOpen() — a remote capture stays open across
     * reconnects (URL is still armed) but isReceivingFrames() flips
     * to false while the decode loop is mid-handshake. The overlay
     * uses this to drive "Connecting / Reconnecting" feedback that
     * the cached captureWidth/Height can't supply (those stay at
     * the last value forever after the stream drops).
     * Local backends return true whenever isOpen() is true — they
     * never hold a "reconnect" state.
     */
    virtual bool isReceivingFrames() const { return isOpen(); }
};

