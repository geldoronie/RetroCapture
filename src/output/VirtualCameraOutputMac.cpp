#include "VirtualCameraOutputMac.h"
#include "VirtcamIpcLayout.h"

#include "../utils/Logger.h"

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

#include <fcntl.h>
#include <mach/mach_time.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <filesystem>

namespace
{

using retrocapture::virtcam_ipc::FrameHeader;
using retrocapture::virtcam_ipc::SharedHeader;
using retrocapture::virtcam_ipc::kFrameMagic;
using retrocapture::virtcam_ipc::kLayoutVersion;
using retrocapture::virtcam_ipc::kMappingSize;
using retrocapture::virtcam_ipc::kSemName;
using retrocapture::virtcam_ipc::kSharedMagic;
using retrocapture::virtcam_ipc::kShmName;
using retrocapture::virtcam_ipc::kSlot0Offset;
using retrocapture::virtcam_ipc::kSlot1Offset;
using retrocapture::virtcam_ipc::slotOffset;

constexpr const char *kDalPluginInstallPath =
    "/Library/CoreMediaIO/Plug-Ins/DAL/RetroCaptureVCam.plugin";

uint8_t *slotPointer(uint8_t *mapView, uint32_t slot)
{
    return mapView + slotOffset(slot);
}

AVPixelFormat toAvPixFmtOut(VirtualCameraOutputMac::PixelFormat f)
{
    switch (f)
    {
        case VirtualCameraOutputMac::PixelFormat::RGBA:  return AV_PIX_FMT_RGBA;
        case VirtualCameraOutputMac::PixelFormat::RGB24: return AV_PIX_FMT_RGB24;
        case VirtualCameraOutputMac::PixelFormat::YUYV:  return AV_PIX_FMT_YUYV422;
        case VirtualCameraOutputMac::PixelFormat::BGRA:  return AV_PIX_FMT_BGRA;
        case VirtualCameraOutputMac::PixelFormat::UYVY:  return AV_PIX_FMT_UYVY422;
    }
    return AV_PIX_FMT_BGRA;
}

size_t bytesPerPixelOut(VirtualCameraOutputMac::PixelFormat f)
{
    switch (f)
    {
        case VirtualCameraOutputMac::PixelFormat::RGBA:  return 4;
        case VirtualCameraOutputMac::PixelFormat::RGB24: return 3;
        case VirtualCameraOutputMac::PixelFormat::YUYV:  return 2;
        case VirtualCameraOutputMac::PixelFormat::BGRA:  return 4;
        case VirtualCameraOutputMac::PixelFormat::UYVY:  return 2;
    }
    return 4;
}

// mach_absolute_time → 100ns units, matching the writer-side
// timestamp the plug-in feeds into CMSampleBufferRef. Computed
// once + cached for performance — every pushFrame goes through
// this on the hot path.
uint64_t machAbs100ns()
{
    static mach_timebase_info_data_t s_tb = {0, 0};
    if (s_tb.denom == 0)
    {
        mach_timebase_info(&s_tb);
    }
    const uint64_t now    = mach_absolute_time();
    const uint64_t nanos  = (now * s_tb.numer) / s_tb.denom;
    return nanos / 100ull;
}

} // namespace

VirtualCameraOutputMac::VirtualCameraOutputMac() = default;

VirtualCameraOutputMac::~VirtualCameraOutputMac()
{
    stop();
}

std::string VirtualCameraOutputMac::lastError() const
{
    std::lock_guard<std::mutex> lk(m_errMu);
    return m_lastError;
}

void VirtualCameraOutputMac::setError(const std::string &err)
{
    std::lock_guard<std::mutex> lk(m_errMu);
    m_lastError = err;
}

bool VirtualCameraOutputMac::isPluginInstalled()
{
    std::error_code ec;
    return std::filesystem::exists(kDalPluginInstallPath, ec);
}

std::string VirtualCameraOutputMac::pluginInstallPath()
{
    return kDalPluginInstallPath;
}

bool VirtualCameraOutputMac::start(uint32_t width, uint32_t height,
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

    // Clear any stale shm from a previous crashed instance — POSIX
    // shm persists past the creating process exit, and re-opening
    // a corrupted region of the wrong size would fail downstream.
    // Best-effort: ignore ENOENT.
    ::shm_unlink(kShmName);

    m_shmFd = ::shm_open(kShmName, O_CREAT | O_RDWR, 0600);
    if (m_shmFd < 0)
    {
        outError = std::string("shm_open failed: ") + std::strerror(errno);
        setError(outError);
        stop();
        return false;
    }
    if (::ftruncate(m_shmFd, static_cast<off_t>(kMappingSize)) < 0)
    {
        outError = std::string("ftruncate failed: ") + std::strerror(errno);
        setError(outError);
        stop();
        return false;
    }
    void *p = ::mmap(nullptr, kMappingSize, PROT_READ | PROT_WRITE,
                     MAP_SHARED, m_shmFd, 0);
    if (p == MAP_FAILED)
    {
        outError = std::string("mmap failed: ") + std::strerror(errno);
        setError(outError);
        stop();
        return false;
    }
    m_mapView = static_cast<uint8_t *>(p);
    m_mapSize = kMappingSize;

    // Zero the header + initialise. Subsequent writers bump
    // writeSlot to 1 on the first frame.
    auto *hdr = reinterpret_cast<SharedHeader *>(m_mapView);
    std::memset(hdr, 0, sizeof(*hdr));
    hdr->magic     = kSharedMagic;
    hdr->version   = kLayoutVersion;
    hdr->writeSlot = 0;
    std::strncpy(hdr->cardLabel, "RetroCapture",
                 sizeof(hdr->cardLabel) - 1);
    for (uint32_t i = 0; i < kSlotCount; ++i)
    {
        auto *fh = reinterpret_cast<FrameHeader *>(
            slotPointer(m_mapView, i));
        std::memset(fh, 0, sizeof(*fh));
    }

    // Auto-reset semaphore semantics: sem_post = signal, sem_wait
    // = consume. Initial value 0 → readers block immediately until
    // the first pushFrame posts. As with shm we unlink any stale
    // sem from a previous crash before creating.
    ::sem_unlink(kSemName);
    sem_t *sem = ::sem_open(kSemName, O_CREAT, 0600, 0);
    if (sem == SEM_FAILED)
    {
        outError = std::string("sem_open failed: ") + std::strerror(errno);
        setError(outError);
        stop();
        return false;
    }
    m_sem = sem;

    m_width      = width;
    m_height     = height;
    m_format     = format;
    m_writeSlot  = 0;
    m_running.store(true);
    {
        std::lock_guard<std::mutex> lk(m_errMu);
        m_lastError.clear();
    }
    LOG_INFO("VirtualCameraOutputMac: opened " +
             std::to_string(width) + "x" + std::to_string(height) +
             " shared mapping; DAL plug-in " +
             (isPluginInstalled() ? "is" : "is NOT") + " installed");
    return true;
}

void VirtualCameraOutputMac::stop()
{
    if (m_mapView)
    {
        ::munmap(m_mapView, m_mapSize);
        m_mapView = nullptr;
        m_mapSize = 0;
    }
    if (m_shmFd >= 0)
    {
        ::close(m_shmFd);
        m_shmFd = -1;
    }
    if (m_sem)
    {
        ::sem_close(static_cast<sem_t *>(m_sem));
        m_sem = nullptr;
    }
    // Note: we deliberately don't shm_unlink / sem_unlink here.
    // The DAL plug-in inside consumer processes may still be holding
    // the mapping open; unlinking would free the name (subsequent
    // shm_open with O_CREAT can recreate) but leave the existing
    // descriptors live until they close. That's fine, and matches
    // the Windows behaviour where CloseHandle is the symmetric op.
    // start() does shm_unlink first to clear stale names from a
    // previous crashed instance.
    freeSws();
    m_convertScratch.clear();
    m_running.store(false);
    m_width = m_height = 0;
    m_writeSlot = 0;
}

bool VirtualCameraOutputMac::ensureSws(uint32_t srcW, uint32_t srcH,
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

void VirtualCameraOutputMac::freeSws()
{
    if (m_sws)
    {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    m_swsSrcW = m_swsSrcH = 0;
    m_swsSrcAvFmt = 0;
}

bool VirtualCameraOutputMac::pushFrame(const uint8_t *pixels,
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
    fh->timestamp100ns  = machAbs100ns();
    fh->payloadBytes    = static_cast<uint32_t>(m_convertScratch.size());
    fh->reserved        = 0;

    std::memcpy(slot + sizeof(FrameHeader),
                m_convertScratch.data(),
                m_convertScratch.size());

    // Atomic release: store writeSlot so a reader that sees the
    // new value is guaranteed to see the FrameHeader + payload.
    auto *hdrSlot = reinterpret_cast<std::atomic<uint32_t> *>(
        &reinterpret_cast<SharedHeader *>(m_mapView)->writeSlot);
    hdrSlot->store(nextSlot, std::memory_order_release);
    m_writeSlot = nextSlot;

    // Wake the reader. sem_post on a sem with value 0 unblocks one
    // sem_wait. If multiple frames pile up while the reader is busy
    // (slow consumer), the sem value will go up — next sem_wait
    // returns immediately and the reader picks up whichever frame
    // is currently in writeSlot (so it may skip intermediate ones,
    // which is the right behaviour for a live source).
    ::sem_post(static_cast<sem_t *>(m_sem));
    return true;
}
