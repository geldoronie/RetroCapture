#pragma once

#include "IVideoCapture.h" // DeviceInfo

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/**
 * Platform backend abstraction for the screen-capture source (#107).
 *
 * VideoCaptureScreen owns one ScreenBackend (PipeWire/portal on Linux,
 * DXGI/WGC on Windows, ScreenCaptureKit on macOS — or a test-pattern
 * stub until those land). The backend grabs desktop frames on its own
 * thread and pushes each one to the sink as packed 32-bit pixels; the
 * sink (VideoCaptureScreen) crops to the region and converts to RGB24
 * for the rest of the pipeline.
 */

enum class ScreenPixelFormat
{
    BGRA, // byte order B,G,R,A (the common PipeWire / DXGI layout)
    BGRX, // B,G,R,unused
    RGBA, // R,G,B,A
    RGBX  // R,G,B,unused
};

// A captured frame delivered as a DMABUF handle (zero-copy GPU buffer)
// instead of CPU pixels. The receiver imports `fd` into a GL texture via
// EGL and OWNS the fd (must close it). drmFourcc/modifier describe the
// buffer layout for EGL_LINUX_DMA_BUF import.
struct ScreenDmabufFrame
{
    int      fd        = -1;
    uint32_t width     = 0;
    uint32_t height    = 0;
    uint32_t stride    = 0;
    uint32_t offset    = 0;
    uint32_t drmFourcc = 0;
    uint64_t modifier  = 0; // DRM_FORMAT_MOD_INVALID when implicit
    bool     hasModifier = false;
};

class IScreenFrameSink
{
public:
    virtual ~IScreenFrameSink() = default;

    /**
     * Deliver one captured frame as CPU pixels. `data` is `height` rows
     * of `stride` bytes; the visible pixels are the first width*4 bytes
     * of each row. Called on the backend's capture thread.
     */
    virtual void onScreenFrame(const uint8_t *data,
                               uint32_t width,
                               uint32_t height,
                               uint32_t stride,
                               ScreenPixelFormat format) = 0;

    /**
     * Deliver one captured frame as a DMABUF (zero-copy). The sink takes
     * ownership of frame.fd. Default ignores it (CPU path only). Called
     * on the backend's capture thread.
     */
    virtual void onScreenDmabuf(const ScreenDmabufFrame &frame) { (void)frame; }
};

class ScreenBackend
{
public:
    virtual ~ScreenBackend() = default;

    /**
     * Begin capturing `target` ("monitor:N" / "window:<id>" / "" for
     * the platform default — on Wayland the portal's own picker decides
     * regardless). Non-blocking: the heavy handshake runs on a worker
     * thread, frames arrive via the sink once it's up. Returns false
     * only on an immediate, unrecoverable setup failure.
     */
    virtual bool start(const std::string &target, bool captureCursor) = 0;

    /** Stop capturing and release all OS resources. Idempotent. */
    virtual void stop() = 0;

    /**
     * Enumerate selectable targets for the UI dropdown. On Wayland the
     * portal drives selection through its own dialog, so this may be a
     * single synthetic entry there.
     */
    virtual std::vector<DeviceInfo> listTargets() { return {}; }
};

// Per-platform factory (one definition compiled per build).
std::unique_ptr<ScreenBackend> createScreenBackend(IScreenFrameSink &sink);
