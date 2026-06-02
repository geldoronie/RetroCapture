#pragma once

#include <cstdint>
#include <cstddef>
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

// Packed 32-bit pixel formats used by the screen-capture source (#107).
// FrameProcessor uploads these straight to GL with GL_BGRA / GL_RGBA and
// lets the driver swizzle to the RGB texture — no CPU colour conversion
// on the hot path, which is what keeps large-monitor capture at full
// frame rate. Sentinel values chosen to not collide with V4L2 fourccs.
static constexpr uint32_t RC_PIXFMT_BGRA = 0xB07A0001u;
static constexpr uint32_t RC_PIXFMT_RGBA = 0xB07A0002u;

struct DeviceInfo
{
    std::string id;        // Device identifier (path, GUID, etc.)
    std::string name;      // Human-readable name
    std::string driver;    // Driver name (optional)
    bool available = true; // Whether device is available
};

// Format descriptor for AVFoundation devices on macOS (each device
// exposes a fixed set of (resolution, fps range, pixel format) tuples;
// picking a format selects all three atomically, OBS-style).
struct AVFoundationFormatInfo
{
    std::string id;          // Unique stable identifier for this format
    uint32_t width = 0;
    uint32_t height = 0;
    float minFps = 0.0f;
    float maxFps = 0.0f;
    std::string pixelFormat; // e.g. "NV12 (420v)", "YUY2 (yuvs)", "BGRA"
    std::string colorSpace;  // e.g. "CS 709"
    std::string displayName; // human-readable concatenation for dropdowns
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

    /**
     * Zero-copy GPU path (#107 screen capture). If the backend can hand
     * the current frame as a ready GL texture (e.g. a DMABUF imported via
     * EGL), it returns the texture id and sets w/h; FrameProcessor then
     * uses it directly and skips the CPU upload. Returns 0 when there's
     * no GPU frame this call (the caller falls back to captureLatestFrame).
     * MUST be called on the GL thread — it may issue GL/EGL calls.
     * The returned texture stays owned by the capture; the caller must
     * not delete it.
     */
    virtual unsigned int getGpuTexture(uint32_t &width, uint32_t &height)
    {
        (void)width; (void)height;
        return 0;
    }

    // AVFoundation-specific extensions. Default no-op so V4L2,
    // DirectShow and Remote captures don't need to implement them.
    virtual std::vector<AVFoundationFormatInfo> listFormats(const std::string &deviceId = "")
    {
        (void)deviceId;
        return {};
    }
    virtual bool setFormatById(const std::string &formatId, const std::string &deviceId = "")
    {
        (void)formatId;
        (void)deviceId;
        return false;
    }

    // Some macOS capture devices (UVC HDMI grabbers etc.) carry audio
    // alongside the video stream — these accessors expose it without
    // forcing a separate IAudioCapture path. Default no-op for
    // platforms whose video pipeline never carries audio.
    virtual bool hasAudio() const { return false; }
    virtual size_t getAudioSamples(int16_t *buffer, size_t maxSamples)
    {
        (void)buffer;
        (void)maxSamples;
        return 0;
    }
    virtual uint32_t getAudioSampleRate() const { return 0; }
    virtual uint32_t getAudioChannels() const { return 0; }
    virtual std::vector<DeviceInfo> listAudioDevices() { return {}; }
    virtual bool setAudioDevice(const std::string &audioDeviceId)
    {
        (void)audioDeviceId;
        return false;
    }
    virtual std::string getCurrentAudioDevice() const { return ""; }
};

