#pragma once

// Host-side virtual-camera sink for Windows. Mirrors the Linux
// `VirtualCameraOutput` interface so Application::syncVirtualCamera
// can be made cross-platform with a typedef once Phase 2 is
// integrated.
//
// This file alone DOES NOT make virtual cameras work on Windows —
// the actual DirectShow source filter lives in a separate DLL
// (src/dshow_filter/) that gets loaded INTO consumer processes
// (OBS, Chrome, Discord). This sink writes frames to a named
// shared memory region + signals a named event; the DLL reads
// from there.
//
// See docs/VIRTCAM_WINDOWS.md for the architecture, IPC layout,
// and the work still needed on the DLL side.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct SwsContext;

class VirtualCameraOutputWin
{
public:
    /// Wire pixel formats the consumer can pick. Maps 1-to-1 to
    /// DirectShow MEDIASUBTYPE_RGB24 / MEDIASUBTYPE_YUY2 over in
    /// the filter side; over in the shared memory header the
    /// enum is serialised as the uint32 pixelFormat field.
    enum class PixelFormat : uint32_t
    {
        RGBA  = 1,
        RGB24 = 2,
        YUYV  = 3,
    };

    /// Source pixel format for pushFrame — mirrors the Linux
    /// sink's enum. Application drains RGBA from PBOManager when
    /// a shader is active and RGB directly from capture
    /// otherwise; the sink converts via sws on its way to the
    /// shared memory slot.
    enum class SourceFormat
    {
        RGBA,
        RGB,
    };

    VirtualCameraOutputWin();
    ~VirtualCameraOutputWin();

    VirtualCameraOutputWin(const VirtualCameraOutputWin &)            = delete;
    VirtualCameraOutputWin &operator=(const VirtualCameraOutputWin &) = delete;

    /// CreateFileMappingW + CreateEventW, format the header,
    /// initialise the two slots. Returns true on success.
    /// outError carries a human-readable reason on failure.
    bool start(uint32_t     width,
               uint32_t     height,
               PixelFormat  format,
               std::string &outError);

    /// Convert + write one frame into the next shared-memory
    /// slot, flip writeSlot, SetEvent. Cheap when dims didn't
    /// change since the last call (sws context is cached).
    bool pushFrame(const uint8_t *pixels,
                   uint32_t       srcWidth,
                   uint32_t       srcHeight,
                   SourceFormat   srcFormat);

    /// Close handles + free sws. Idempotent.
    void stop();

    bool isRunning() const { return m_running.load(); }
    uint32_t outputWidth()  const { return m_width; }
    uint32_t outputHeight() const { return m_height; }
    PixelFormat outputFormat() const { return m_format; }

    std::string lastError() const;

    /// Whether the DirectShow filter DLL is registered on this
    /// machine. Checks the CLSID registry key under HKCR\CLSID.
    /// Used by the UI to decide whether to show the "Install
    /// virtcam driver" button vs the "Start Capture" button.
    static bool isFilterDllRegistered();

    // Shared mapping layout constants — public because the .cpp's
    // anonymous-namespace helpers (slotPointer, kSlot0Offset, etc.)
    // reference them at file scope, and the DLL side will read
    // the same constants out of the matching header on Windows.
    // See docs/VIRTCAM_WINDOWS.md "Shared memory layout".
    static constexpr uint32_t kSlotMaxBytes = 3840u * 2160u * 4u;
    static constexpr uint32_t kSlotCount    = 2;

private:
    bool ensureSws(uint32_t srcW, uint32_t srcH, SourceFormat srcFmt);
    void freeSws();
    void setError(const std::string &err);

    void *m_mapHandle    = nullptr;  // HANDLE — void* to avoid windows.h in the header
    void *m_eventHandle  = nullptr;  // HANDLE — auto-reset event
    uint8_t *m_mapView   = nullptr;  // MapViewOfFile result
    size_t   m_mapSize   = 0;

    uint32_t    m_width  = 0;
    uint32_t    m_height = 0;
    PixelFormat m_format = PixelFormat::RGBA;
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
