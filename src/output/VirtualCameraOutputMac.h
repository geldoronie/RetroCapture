#pragma once

// Host-side virtual-camera sink for macOS. Mirrors the Linux
// `VirtualCameraOutput` / Windows `VirtualCameraOutputWin`
// interfaces so the cross-platform typedef in Application.h can
// pick whichever one the target build needs.
//
// This file alone DOES NOT make virtual cameras work on macOS —
// the actual CoreMediaIO DAL plug-in lives in a separate bundle
// (src/dal_plugin/) that gets loaded INTO every consumer process
// (OBS, Chrome, Discord, Zoom...) via /Library/CoreMediaIO/Plug-Ins/DAL/.
// This sink writes frames to a POSIX shared-memory region + signals
// a named semaphore; the bundle reads from there.
//
// See docs/VIRTCAM_MACOS.md (TODO — design doc per Windows) for the
// architecture, IPC layout, and the work still needed on the
// plug-in side.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct SwsContext;

class VirtualCameraOutputMac
{
public:
    /// Wire pixel formats the consumer can pick. Maps 1-to-1 to
    /// CMIO format descriptors (CMPixelFormatType k32BGRA / 422YpCbCr8)
    /// over in the plugin side; over in the shared-memory header
    /// the enum is serialised as the uint32 pixelFormat field.
    enum class PixelFormat : uint32_t
    {
        RGBA  = 1,
        RGB24 = 2,
        YUYV  = 3,
    };

    /// Source pixel format for pushFrame — mirrors the other sinks'
    /// enum. Application drains RGBA from PBOManager when a shader
    /// is active and RGB directly from capture otherwise; the sink
    /// converts via sws on its way to the shared memory slot.
    enum class SourceFormat
    {
        RGBA,
        RGB,
    };

    VirtualCameraOutputMac();
    ~VirtualCameraOutputMac();

    VirtualCameraOutputMac(const VirtualCameraOutputMac &)            = delete;
    VirtualCameraOutputMac &operator=(const VirtualCameraOutputMac &) = delete;

    /// shm_open + ftruncate + mmap + sem_open. Returns true on
    /// success. outError carries a human-readable reason on failure.
    bool start(uint32_t     width,
               uint32_t     height,
               PixelFormat  format,
               std::string &outError);

    /// Convert + write one frame into the next shared-memory
    /// slot, atomic-flip writeSlot with release semantics,
    /// sem_post. Cheap when dims didn't change since the last
    /// call (sws context is cached).
    bool pushFrame(const uint8_t *pixels,
                   uint32_t       srcWidth,
                   uint32_t       srcHeight,
                   SourceFormat   srcFormat);

    /// munmap + close (no shm_unlink — plugin may still be reading).
    /// Idempotent.
    void stop();

    bool        isRunning()    const { return m_running.load(); }
    uint32_t    outputWidth()  const { return m_width; }
    uint32_t    outputHeight() const { return m_height; }
    PixelFormat outputFormat() const { return m_format; }

    std::string lastError() const;

    /// Whether the CoreMediaIO DAL plug-in is installed on this
    /// machine. Checks /Library/CoreMediaIO/Plug-Ins/DAL/RetroCaptureVCam.plugin
    /// existence. Used by the UI to decide whether to show the
    /// "Install virtcam plug-in" button vs the "Start Capture" button.
    static bool isPluginInstalled();

    /// Canonical install path for the plug-in. Surfaced in the UI
    /// so a power user knows where to drop the bundle manually
    /// (or to verify after install).
    static std::string pluginInstallPath();

    // Shared mapping layout constants — public because the .cpp's
    // anonymous-namespace helpers reference them at file scope, and
    // the plugin side will read the same constants out of the
    // matching header.
    static constexpr uint32_t kSlotMaxBytes = 3840u * 2160u * 4u;
    static constexpr uint32_t kSlotCount    = 2;

private:
    bool ensureSws(uint32_t srcW, uint32_t srcH, SourceFormat srcFmt);
    void freeSws();
    void setError(const std::string &err);

    int      m_shmFd     = -1;
    uint8_t *m_mapView   = nullptr;
    size_t   m_mapSize   = 0;
    void    *m_sem       = nullptr;  // sem_t* — void to keep semaphore.h out of the header

    uint32_t    m_width     = 0;
    uint32_t    m_height    = 0;
    PixelFormat m_format    = PixelFormat::RGBA;
    uint32_t    m_writeSlot = 0;

    SwsContext *m_sws         = nullptr;
    uint32_t    m_swsSrcW     = 0;
    uint32_t    m_swsSrcH     = 0;
    int         m_swsSrcAvFmt = 0;

    std::vector<uint8_t> m_convertScratch;

    std::atomic<bool>  m_running{false};
    mutable std::mutex m_errMu;
    std::string        m_lastError;
};
