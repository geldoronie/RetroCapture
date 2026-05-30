#pragma once

// Shared on-wire layout for the Windows virtual-camera IPC.
// The host side (`VirtualCameraOutputWin`) and the filter side
// (`RetroCaptureVCam.dll`'s `SharedMemoryReader`) both include
// this header to guarantee the structs and offsets stay in sync.
// A drift here = host writes one layout, filter reads another =
// no frames or torn frames. Centralising prevents that class of
// bug at the cost of one shared include.
//
// See docs/VIRTCAM_WINDOWS.md for the full architecture.

#include <cstddef>
#include <cstdint>

namespace retrocapture { namespace virtcam_ipc {

// `inline` for variables is C++17 only and the MXE/MinGW used for
// the Windows cross-build is GCC 5.5 (predates it). Plain
// `constexpr` at namespace scope is safe here because the constants
// are used as values, not ODR-used (no address-taken, no const-ref
// bind). If a TU ever needs &kSharedMagic etc., promote to inline.

// Named objects — both versioned with `_v1` so the day the layout
// changes incompatibly we can bump the suffix and old + new
// RetroCapture instances coexist without colliding on the same
// names. On Windows the names are wide-char + `Local\` prefixed
// (per-session scope so two users on the same machine get
// independent virtual cameras). On macOS the equivalents are
// POSIX names that have to start with `/` and stay under
// PSHMNAMLEN (~31 chars on most Macs) — chosen short.
#if defined(_WIN32)
constexpr wchar_t kMapName[]   = L"Local\\RetroCaptureVCam_FrameMap_v1";
constexpr wchar_t kEventName[] = L"Local\\RetroCaptureVCam_FrameReady_v1";
#elif defined(__APPLE__)
// Per-user scope would be nicer but `Local\`-equivalent on macOS
// (per-user `shm_open`) requires baking the user id into the name.
// Keeping system-wide for now matches OBS Virtual Cam's behaviour
// and is simple.
constexpr char    kShmName[]   = "/RCVcamShm_v1";
constexpr char    kSemName[]   = "/RCVcamEvt_v1";
#endif

// Magic numbers double as torn-write guards. A reader that sees
// 0x00000000 in either field knows the writer hasn't laid down
// anything valid in this slot yet.
constexpr uint32_t kSharedMagic = 0x52435643u; // 'RCVC'
constexpr uint32_t kFrameMagic  = 0x52434652u; // 'RCFR'
constexpr uint32_t kLayoutVersion = 1u;

// On-wire pixel format. Stored as uint32 in FrameHeader.pixelFormat
// so the filter side doesn't need to share the enum class. The
// writer-side enum (`VirtualCameraOutputWin::PixelFormat`) maps
// 1-to-1 to these values; keep them stable.
constexpr uint32_t kPixelFormatRGBA  = 1u;
constexpr uint32_t kPixelFormatRGB24 = 2u;
constexpr uint32_t kPixelFormatYUYV  = 3u;
// BGRA — byte order B,G,R,A. Added for the macOS DAL path: maps
// 1:1 to kCVPixelFormatType_32BGRA, the most widely-rendered
// uncompressed format across CMIO consumers (24RGB, which we tried
// first, is poorly supported and showed the device but no image).
constexpr uint32_t kPixelFormatBGRA  = 4u;

// Slot geometry — fixed so the filter can address slots without
// computing offsets from runtime fields. `kSlotMaxBytes` is sized
// for 4K RGBA + the per-frame header. The writer always uses two
// slots so a reader picking up frame N while the writer lays down
// N+1 doesn't tear (writer flips writeSlot AFTER the payload is in).
constexpr uint32_t kSlotMaxBytes = 3840u * 2160u * 4u;
constexpr uint32_t kSlotCount    = 2u;

#pragma pack(push, 1)

// Mapping header — first 96 bytes of the file mapping.
struct SharedHeader
{
    uint32_t magic;          // kSharedMagic
    uint32_t version;        // kLayoutVersion
    uint32_t writeSlot;      // 0 or 1 — the slot the writer last completed
    uint32_t reserved0;
    char     cardLabel[64];  // UTF-8, null-terminated friendly name
    uint32_t reserved1[4];
};
static_assert(sizeof(SharedHeader) == 96, "SharedHeader must be 96 bytes");

// Per-slot header — followed immediately by `payloadBytes` of pixels.
struct FrameHeader
{
    uint32_t magic;           // kFrameMagic
    uint32_t width;
    uint32_t height;
    uint32_t pixelFormat;     // one of kPixelFormat*
    uint64_t timestamp100ns;  // QueryPerformanceCounter-derived, host monotonic
    uint32_t payloadBytes;    // width * height * bytesPerPixel(pixelFormat)
    uint32_t reserved;
};
static_assert(sizeof(FrameHeader) == 32, "FrameHeader must be 32 bytes");

#pragma pack(pop)

constexpr std::size_t kSlotSize    = sizeof(FrameHeader) + kSlotMaxBytes;
constexpr std::size_t kSlot0Offset = sizeof(SharedHeader);
constexpr std::size_t kSlot1Offset = sizeof(SharedHeader) + kSlotSize;
constexpr std::size_t kMappingSize = sizeof(SharedHeader) + kSlotSize * kSlotCount;

// Offset for slot index 0 or 1. Behaviour for any other index is
// undefined — assert in callers that bound-check.
constexpr std::size_t slotOffset(uint32_t slot)
{
    return slot == 0 ? kSlot0Offset : kSlot1Offset;
}

}} // namespace retrocapture::virtcam_ipc
