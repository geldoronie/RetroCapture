#include "VirtualCameraOutputWin.h"

#include "../utils/Logger.h"

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

// Win32 — we use file mapping, events, and the CLSID registry
// lookup helper. Defined BEFORE windows.h so we don't drag in
// the bulk of the headers (winsock, gdi, etc).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstring>

namespace
{
// Named-object names. The "Local\" prefix scopes them per session
// — two users on the same Windows host get independent virtual
// cameras. _v1 is a forward compat hook: bump if the layout
// changes incompatibly.
constexpr wchar_t kMapName[]    = L"Local\\RetroCaptureVCam_FrameMap_v1";
constexpr wchar_t kEventName[]  = L"Local\\RetroCaptureVCam_FrameReady_v1";

// Magic numbers in the header — also serve as torn-write guards
// (a reader that sees 0x00000000 knows the writer hasn't laid
// down a frame yet).
constexpr uint32_t kSharedMagic = 0x52435643u; // 'RCVC'
constexpr uint32_t kFrameMagic  = 0x52434652u; // 'RCFR'

// CLSID of the DirectShow filter DLL — see docs/VIRTCAM_WINDOWS.md.
// Used by isFilterDllRegistered() to probe the registry.
constexpr wchar_t kFilterClsidKey[] =
    L"CLSID\\{C4F2E1A0-7B3D-4F8E-9C1B-RC850000VCAM}";

#pragma pack(push, 1)
struct SharedHeader
{
    uint32_t magic;        // kSharedMagic
    uint32_t version;      // 1
    uint32_t writeSlot;    // 0 or 1 — most-recently-completed slot
    uint32_t reserved0;
    char     cardLabel[64];
    uint32_t reserved1[4];
};
static_assert(sizeof(SharedHeader) == 96, "SharedHeader must be 96 bytes");

struct FrameHeader
{
    uint32_t magic;        // kFrameMagic
    uint32_t width;
    uint32_t height;
    uint32_t pixelFormat;  // VirtualCameraOutputWin::PixelFormat
    uint64_t timestamp100ns;
    uint32_t payloadBytes;
    uint32_t reserved;
};
static_assert(sizeof(FrameHeader) == 32, "FrameHeader must be 32 bytes");
#pragma pack(pop)

constexpr size_t kSlotSize    = sizeof(FrameHeader) + VirtualCameraOutputWin::kSlotMaxBytes;
constexpr size_t kSlot0Offset = sizeof(SharedHeader);
constexpr size_t kSlot1Offset = sizeof(SharedHeader) + kSlotSize;
constexpr size_t kMappingSize = sizeof(SharedHeader) +
                                kSlotSize * VirtualCameraOutputWin::kSlotCount;

uint8_t *slotPointer(uint8_t *mapView, uint32_t slot)
{
    return mapView + (slot == 0 ? kSlot0Offset : kSlot1Offset);
}

AVPixelFormat toAvPixFmtOut(VirtualCameraOutputWin::PixelFormat f)
{
    switch (f)
    {
        case VirtualCameraOutputWin::PixelFormat::RGBA:  return AV_PIX_FMT_RGBA;
        case VirtualCameraOutputWin::PixelFormat::RGB24: return AV_PIX_FMT_RGB24;
        case VirtualCameraOutputWin::PixelFormat::YUYV:  return AV_PIX_FMT_YUYV422;
    }
    return AV_PIX_FMT_RGBA;
}

size_t bytesPerPixelOut(VirtualCameraOutputWin::PixelFormat f)
{
    switch (f)
    {
        case VirtualCameraOutputWin::PixelFormat::RGBA:  return 4;
        case VirtualCameraOutputWin::PixelFormat::RGB24: return 3;
        case VirtualCameraOutputWin::PixelFormat::YUYV:  return 2;
    }
    return 4;
}

// QueryPerformanceCounter → 100ns units, matching IReferenceClock /
// IMediaSample::SetTime which DirectShow consumers expect.
uint64_t qpc100ns()
{
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    // 100 ns = 1e7 Hz. (now * 1e7) / freq.
    return static_cast<uint64_t>(
        (static_cast<double>(now.QuadPart) * 1.0e7) /
        static_cast<double>(freq.QuadPart));
}
} // namespace

VirtualCameraOutputWin::VirtualCameraOutputWin() = default;

VirtualCameraOutputWin::~VirtualCameraOutputWin()
{
    stop();
}

void VirtualCameraOutputWin::setError(const std::string &err)
{
    std::lock_guard<std::mutex> lk(m_errMu);
    m_lastError = err;
}

std::string VirtualCameraOutputWin::lastError() const
{
    std::lock_guard<std::mutex> lk(m_errMu);
    return m_lastError;
}

bool VirtualCameraOutputWin::isFilterDllRegistered()
{
    HKEY key;
    const LONG rc = ::RegOpenKeyExW(HKEY_CLASSES_ROOT, kFilterClsidKey,
                                     0, KEY_READ, &key);
    if (rc != ERROR_SUCCESS) return false;
    ::RegCloseKey(key);
    return true;
}

bool VirtualCameraOutputWin::start(uint32_t width, uint32_t height,
                                    PixelFormat format,
                                    std::string &outError)
{
    if (m_running.load() && m_width == width && m_height == height &&
        m_format == format)
    {
        return true;
    }
    stop();

    if (width == 0 || height == 0)
    {
        outError = "width/height must be non-zero";
        setError(outError);
        return false;
    }
    if (static_cast<size_t>(width) * height *
        bytesPerPixelOut(format) > kSlotMaxBytes)
    {
        outError = "frame size exceeds the 4K-RGBA cap baked into "
                   "the shared-memory layout";
        setError(outError);
        return false;
    }

    // Create-or-open semantics: another instance of RetroCapture
    // may have left the mapping alive briefly; OpenFileMapping
    // would attach to a stale one. Always Create — Windows
    // returns the existing handle if the section is still alive,
    // with GetLastError == ERROR_ALREADY_EXISTS. We accept that.
    m_mapHandle = ::CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(kMappingSize), kMapName);
    if (!m_mapHandle)
    {
        outError = "CreateFileMappingW failed: GetLastError=" +
                   std::to_string(::GetLastError());
        setError(outError);
        stop();
        return false;
    }
    m_mapView = static_cast<uint8_t *>(
        ::MapViewOfFile(m_mapHandle, FILE_MAP_ALL_ACCESS,
                        0, 0, kMappingSize));
    if (!m_mapView)
    {
        outError = "MapViewOfFile failed: GetLastError=" +
                   std::to_string(::GetLastError());
        setError(outError);
        stop();
        return false;
    }
    m_mapSize = kMappingSize;

    // Zero the header + clear writeSlot to 0; subsequent writers
    // bump it to 1 on the first frame.
    auto *hdr = reinterpret_cast<SharedHeader *>(m_mapView);
    std::memset(hdr, 0, sizeof(*hdr));
    hdr->magic     = kSharedMagic;
    hdr->version   = 1;
    hdr->writeSlot = 0;
    std::strncpy(hdr->cardLabel, "RetroCapture",
                 sizeof(hdr->cardLabel) - 1);
    // Mark both slot headers as empty so a reader that races
    // ahead of the first SetEvent doesn't pick up garbage.
    for (uint32_t i = 0; i < kSlotCount; ++i)
    {
        auto *fh = reinterpret_cast<FrameHeader *>(slotPointer(m_mapView, i));
        std::memset(fh, 0, sizeof(*fh));
    }

    m_eventHandle = ::CreateEventW(nullptr, /*manualReset=*/FALSE,
                                    /*initialState=*/FALSE, kEventName);
    if (!m_eventHandle)
    {
        outError = "CreateEventW failed: GetLastError=" +
                   std::to_string(::GetLastError());
        setError(outError);
        stop();
        return false;
    }

    m_width      = width;
    m_height     = height;
    m_format     = format;
    m_writeSlot  = 0;
    m_running.store(true);
    {
        std::lock_guard<std::mutex> lk(m_errMu);
        m_lastError.clear();
    }
    LOG_INFO("VirtualCameraOutputWin: opened " +
             std::to_string(width) + "x" + std::to_string(height) +
             " shared mapping; filter DLL " +
             (isFilterDllRegistered() ? "is" : "is NOT") +
             " registered");
    return true;
}

void VirtualCameraOutputWin::stop()
{
    if (m_mapView)
    {
        ::UnmapViewOfFile(m_mapView);
        m_mapView = nullptr;
    }
    if (m_mapHandle)
    {
        ::CloseHandle(m_mapHandle);
        m_mapHandle = nullptr;
    }
    if (m_eventHandle)
    {
        ::CloseHandle(m_eventHandle);
        m_eventHandle = nullptr;
    }
    freeSws();
    m_convertScratch.clear();
    m_running.store(false);
    m_width = m_height = 0;
    m_writeSlot = 0;
}

bool VirtualCameraOutputWin::ensureSws(uint32_t srcW, uint32_t srcH,
                                        SourceFormat srcFmt)
{
    const AVPixelFormat avSrc = (srcFmt == SourceFormat::RGBA)
        ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24;
    if (m_sws && m_swsSrcW == srcW && m_swsSrcH == srcH &&
        m_swsSrcAvFmt == static_cast<int>(avSrc))
    {
        return true;
    }
    freeSws();
    m_sws = sws_getContext(
        static_cast<int>(srcW), static_cast<int>(srcH), avSrc,
        static_cast<int>(m_width), static_cast<int>(m_height),
        toAvPixFmtOut(m_format),
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_sws)
    {
        setError("sws_getContext failed");
        return false;
    }
    m_swsSrcW     = srcW;
    m_swsSrcH     = srcH;
    m_swsSrcAvFmt = static_cast<int>(avSrc);
    m_convertScratch.resize(static_cast<size_t>(m_width) * m_height *
                             bytesPerPixelOut(m_format));
    return true;
}

void VirtualCameraOutputWin::freeSws()
{
    if (m_sws)
    {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    m_swsSrcW = m_swsSrcH = 0;
    m_swsSrcAvFmt = 0;
}

bool VirtualCameraOutputWin::pushFrame(const uint8_t *pixels,
                                        uint32_t srcWidth, uint32_t srcHeight,
                                        SourceFormat srcFormat)
{
    if (!m_running.load() || !pixels) return false;
    if (!ensureSws(srcWidth, srcHeight, srcFormat)) return false;

    // Convert + rescale into scratch.
    const int srcStridePx = (srcFormat == SourceFormat::RGBA) ? 4 : 3;
    const uint8_t *srcSlice[1]  = { pixels };
    int            srcStride[1] = { static_cast<int>(srcWidth) * srcStridePx };
    uint8_t       *dstSlice[1]  = { m_convertScratch.data() };
    int            dstStride[1] = {
        static_cast<int>(m_width) *
        static_cast<int>(bytesPerPixelOut(m_format)) };
    sws_scale(m_sws, srcSlice, srcStride, 0,
              static_cast<int>(srcHeight), dstSlice, dstStride);

    // Pick the NEXT slot — opposite of what the reader most
    // recently consumed.
    const uint32_t nextSlot = (m_writeSlot + 1) & 1;
    uint8_t *slot = slotPointer(m_mapView, nextSlot);
    auto    *fh   = reinterpret_cast<FrameHeader *>(slot);

    // Lay down the header BEFORE the payload, but advertise via
    // SharedHeader.writeSlot only AFTER the payload is in. The
    // reader synchronises on writeSlot.
    fh->magic           = kFrameMagic;
    fh->width           = m_width;
    fh->height          = m_height;
    fh->pixelFormat     = static_cast<uint32_t>(m_format);
    fh->timestamp100ns  = qpc100ns();
    fh->payloadBytes    = static_cast<uint32_t>(m_convertScratch.size());
    fh->reserved        = 0;

    std::memcpy(slot + sizeof(FrameHeader),
                m_convertScratch.data(),
                m_convertScratch.size());

    // Atomic release: store writeSlot so a reader that sees the
    // new value is guaranteed to see the FrameHeader + payload.
    // Reinterpret as atomic<uint32_t> for the store.
    auto *hdrSlot = reinterpret_cast<std::atomic<uint32_t> *>(
        &reinterpret_cast<SharedHeader *>(m_mapView)->writeSlot);
    hdrSlot->store(nextSlot, std::memory_order_release);
    m_writeSlot = nextSlot;

    // Wake the reader. Auto-reset event — no need to clear it
    // here; the reader's WaitForSingleObject does that.
    ::SetEvent(m_eventHandle);
    return true;
}
