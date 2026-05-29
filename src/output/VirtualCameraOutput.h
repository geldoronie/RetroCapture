#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// Forward decl — libswscale forward-declares its context as a struct
// pointer, same trick MediaEncoder uses.
struct SwsContext;

/**
 * Linux v4l2loopback output sink — Phase 1 of #85.
 *
 * Publishes RetroCapture's processed RGBA framebuffer as a
 * V4L2 OUTPUT device that downstream apps (OBS, Chrome's
 * getUserMedia, Discord, Zoom, …) can pick as a webcam source.
 *
 * Architecture: piggybacks on the existing PBO readback that
 * streaming + recording already pay for — the per-frame work added
 * here is ONLY the colour conversion (RGBA → YUYV or RGB24) plus
 * a v4l2 QBUF ioctl. No extra GL roundtrips, no extra glReadPixels.
 *
 * Lifecycle:
 *   - Construct (cheap, no IO).
 *   - start(devicePath, w, h, fps, format) — opens the device,
 *     negotiates format, mmaps output buffers, transitions to
 *     V4L2 STREAMING. Idempotent on the same parameters; if any
 *     differ from a previous start() the device is torn down and
 *     reopened.
 *   - pushFrame(rgba, w, h) — convert + enqueue ONE frame. Called
 *     once per render tick by Application after the PBO readback.
 *     Cheap when sws context dims already match the input; rebuilds
 *     it on resize.
 *   - stop() — STREAMOFF, munmap, close. Other apps see "camera
 *     disconnected" cleanly.
 *
 * Thread model: all members are touched only from the render
 * thread (Application's main loop). No internal threads. The
 * v4l2 kernel side does its own buffering — we just QBUF and
 * the consumer DQBUFs.
 */
class VirtualCameraOutput
{
public:
    /// Wire format we hand to the v4l2loopback device. Both are
    /// widely accepted by downstream consumers; YUYV is the default
    /// because every browser + meeting app supports it without
    /// the YUV-from-RGB conversion needing to happen consumer-side.
    enum class PixelFormat
    {
        YUYV,    // V4L2_PIX_FMT_YUYV — 4:2:2 packed, 16 bpp.
        RGB24,   // V4L2_PIX_FMT_RGB24 — 24 bpp, no chroma loss.
    };

    /// Device descriptor returned by enumerateDevices().
    struct DeviceInfo
    {
        std::string path;       // "/dev/video10"
        std::string cardLabel;  // human-readable, from VIDIOC_QUERYCAP
    };

    /// Scan /dev/video* for v4l2loopback OUTPUT devices. Filters
    /// by driver name ("v4l2 loopback") AND V4L2_CAP_VIDEO_OUTPUT.
    /// Returns an empty vector when the kernel module isn't loaded.
    /// Cheap — one VIDIOC_QUERYCAP per /dev/video* node, runs in a
    /// few ms even with many entries.
    static std::vector<DeviceInfo> enumerateDevices();

    VirtualCameraOutput();
    ~VirtualCameraOutput();

    VirtualCameraOutput(const VirtualCameraOutput &)            = delete;
    VirtualCameraOutput &operator=(const VirtualCameraOutput &) = delete;

    /// Open + configure + start streaming. width/height are the
    /// frame dims we'll publish (frames pushed in pushFrame are
    /// sws-rescaled to this size if they don't match). fps is the
    /// declared framerate (cosmetic for the consumer's UI — kernel
    /// doesn't enforce pacing on OUTPUT devices).
    ///
    /// Returns true on success. On failure outError carries a
    /// human-readable reason ("no such device", "format rejected",
    /// "REQBUFS failed", etc.) — caller surfaces it inline.
    bool start(const std::string &devicePath,
               uint32_t           width,
               uint32_t           height,
               uint32_t           fps,
               PixelFormat        format,
               std::string       &outError);

    /// Drop a frame into the next available output buffer.
    /// rgbaWidth / rgbaHeight describe the SOURCE; if they differ
    /// from the device's negotiated dims we sws_scale to the
    /// device dims. Stride is assumed packed (rgbaWidth*4 bytes
    /// per row) — matches PBOManager's output.
    ///
    /// Safe to call at the source FPS; the kernel drops backed-up
    /// frames so a slow consumer doesn't backpressure us.
    /// Returns false on a hard error (device gone, ioctl failed
    /// repeatedly); caller can decide whether to stop().
    bool pushFrame(const uint8_t *rgba,
                   uint32_t       rgbaWidth,
                   uint32_t       rgbaHeight);

    /// VIDIOC_STREAMOFF + munmap + close. Safe to call when not
    /// started; idempotent.
    void stop();

    /// True between a successful start() and stop().
    bool isRunning() const { return m_running.load(); }

    /// Last error string set by start() or pushFrame(). Cleared on
    /// the next successful start().
    std::string lastError() const;

    /// Negotiated output dims + format. Useful for the UI status
    /// line ("publishing 1280x720 YUYV to /dev/video10").
    uint32_t    outputWidth()  const { return m_outWidth; }
    uint32_t    outputHeight() const { return m_outHeight; }
    PixelFormat outputFormat() const { return m_outFormat; }
    const std::string &devicePath() const { return m_devicePath; }

private:
    // V4L2 output buffer descriptor — mirrors the input-side struct
    // in VideoCaptureV4L2. Plain POD so we can vector<> them.
    struct OutBuffer
    {
        uint8_t *data   = nullptr; // mmap base
        size_t   length = 0;       // mmap length
    };

    // Helpers (impl details, defined in .cpp).
    bool negotiateFormat(uint32_t w, uint32_t h, PixelFormat fmt,
                         std::string &outError);
    bool requestAndMapBuffers(std::string &outError);
    void unmapBuffers();
    bool ensureSws(uint32_t srcW, uint32_t srcH);
    void freeSws();

    int               m_fd          = -1;
    std::string       m_devicePath;
    uint32_t          m_outWidth    = 0;
    uint32_t          m_outHeight   = 0;
    uint32_t          m_outFps      = 0;
    PixelFormat       m_outFormat   = PixelFormat::YUYV;
    std::atomic<bool> m_running{false};

    std::vector<OutBuffer> m_buffers;

    // sws conversion state — rebuilt on input-size change. swsSrcW
    // == 0 means "uninitialised, will lazy-build on first push".
    SwsContext *m_sws     = nullptr;
    uint32_t    m_swsSrcW = 0;
    uint32_t    m_swsSrcH = 0;

    // Scratch buffer the converted frame lives in before the
    // memcpy into the mmap'd output buffer. Single-allocated on
    // first push, reused thereafter.
    std::vector<uint8_t> m_convertScratch;

    mutable std::mutex m_errMu;
    std::string        m_lastError;

    void setError(const std::string &err);
};
