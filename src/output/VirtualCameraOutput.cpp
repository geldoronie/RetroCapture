#include "VirtualCameraOutput.h"

#include "../utils/Logger.h"

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
// /dev nodes V4L2 might use. We only look at /dev/video*; v4l2loopback
// devices register here too.
const char *kDevDir       = "/dev";
const char *kDevPrefix    = "video";
// Driver string v4l2loopback reports in cap.driver. The trailing
// underscore is just defensive — the actual driver string is
// "v4l2 loopback", we substring-match.
const char *kLoopbackDrv  = "v4l2 loopback";

int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ::ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

uint32_t toV4l2Fourcc(VirtualCameraOutput::PixelFormat f)
{
    switch (f)
    {
        case VirtualCameraOutput::PixelFormat::YUYV:  return V4L2_PIX_FMT_YUYV;
        case VirtualCameraOutput::PixelFormat::RGB24: return V4L2_PIX_FMT_RGB24;
    }
    return V4L2_PIX_FMT_YUYV;
}

AVPixelFormat toAvPixFmt(VirtualCameraOutput::PixelFormat f)
{
    switch (f)
    {
        case VirtualCameraOutput::PixelFormat::YUYV:  return AV_PIX_FMT_YUYV422;
        case VirtualCameraOutput::PixelFormat::RGB24: return AV_PIX_FMT_RGB24;
    }
    return AV_PIX_FMT_YUYV422;
}

// Bytes-per-pixel for our supported output formats. YUYV is
// packed 4:2:2 (16 bpp = 2 bytes/pixel); RGB24 is 24 bpp = 3.
size_t bytesPerPixel(VirtualCameraOutput::PixelFormat f)
{
    switch (f)
    {
        case VirtualCameraOutput::PixelFormat::YUYV:  return 2;
        case VirtualCameraOutput::PixelFormat::RGB24: return 3;
    }
    return 2;
}
} // namespace

// ---- Static device enumeration -------------------------------------

std::vector<VirtualCameraOutput::DeviceInfo>
VirtualCameraOutput::enumerateDevices()
{
    std::vector<DeviceInfo> out;
    DIR *d = ::opendir(kDevDir);
    if (!d) return out;
    struct dirent *ent;
    while ((ent = ::readdir(d)) != nullptr)
    {
        const std::string name = ent->d_name;
        if (name.rfind(kDevPrefix, 0) != 0) continue; // not "video*"
        const std::string path = std::string(kDevDir) + "/" + name;

        // O_NONBLOCK so a busy device doesn't stall the scan; we
        // only need QUERYCAP which is a cheap synchronous ioctl.
        int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;

        v4l2_capability cap{};
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
        {
            const std::string driver(
                reinterpret_cast<const char *>(cap.driver));
            // v4l2loopback nodes advertise BOTH capture + output on
            // the same fd. We need the output bit.
            const uint32_t caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS
                ? cap.device_caps
                : cap.capabilities;
            const bool isLoopback = driver.find(kLoopbackDrv) != std::string::npos;
            const bool canOutput  = (caps & V4L2_CAP_VIDEO_OUTPUT) != 0;
            if (isLoopback && canOutput)
            {
                DeviceInfo info;
                info.path      = path;
                info.cardLabel = reinterpret_cast<const char *>(cap.card);
                out.push_back(std::move(info));
            }
        }
        ::close(fd);
    }
    ::closedir(d);
    // Stable ordering — sort by path so /dev/video10 < /dev/video11.
    std::sort(out.begin(), out.end(),
              [](const DeviceInfo &a, const DeviceInfo &b) {
                  return a.path < b.path;
              });
    return out;
}

// ---- Construction --------------------------------------------------

VirtualCameraOutput::VirtualCameraOutput() = default;

VirtualCameraOutput::~VirtualCameraOutput()
{
    stop();
}

void VirtualCameraOutput::setError(const std::string &err)
{
    std::lock_guard<std::mutex> lk(m_errMu);
    m_lastError = err;
}

std::string VirtualCameraOutput::lastError() const
{
    std::lock_guard<std::mutex> lk(m_errMu);
    return m_lastError;
}

// ---- Format negotiation -------------------------------------------

bool VirtualCameraOutput::negotiateFormat(uint32_t w, uint32_t h,
                                          PixelFormat fmt,
                                          std::string &outError)
{
    v4l2_format f{};
    f.type                 = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    f.fmt.pix.width        = w;
    f.fmt.pix.height       = h;
    f.fmt.pix.pixelformat  = toV4l2Fourcc(fmt);
    f.fmt.pix.field        = V4L2_FIELD_NONE;
    f.fmt.pix.bytesperline = static_cast<uint32_t>(w * bytesPerPixel(fmt));
    f.fmt.pix.sizeimage    = f.fmt.pix.bytesperline * h;
    f.fmt.pix.colorspace   = V4L2_COLORSPACE_SRGB;

    if (xioctl(m_fd, VIDIOC_S_FMT, &f) < 0)
    {
        outError = std::string("VIDIOC_S_FMT failed: ") + std::strerror(errno);
        return false;
    }
    // The driver may have rewritten our params (loopback usually
    // accepts whatever we ask). Record what actually got negotiated.
    m_outWidth  = f.fmt.pix.width;
    m_outHeight = f.fmt.pix.height;
    m_outFormat = fmt;
    return true;
}

// ---- Buffer alloc + mmap ------------------------------------------

bool VirtualCameraOutput::requestAndMapBuffers(std::string &outError)
{
    // Two buffers is plenty for OUTPUT — one queued, one being
    // filled. v4l2loopback doesn't care about deep queues since
    // it just serves the freshest frame to consumers.
    constexpr uint32_t kBufferCount = 2;

    v4l2_requestbuffers req{};
    req.count  = kBufferCount;
    req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(m_fd, VIDIOC_REQBUFS, &req) < 0)
    {
        outError = std::string("VIDIOC_REQBUFS failed: ") + std::strerror(errno);
        return false;
    }
    if (req.count == 0)
    {
        outError = "driver allocated 0 output buffers";
        return false;
    }

    m_buffers.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i)
    {
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (xioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0)
        {
            outError = std::string("VIDIOC_QUERYBUF[") +
                       std::to_string(i) + "] failed: " + std::strerror(errno);
            return false;
        }
        void *p = ::mmap(nullptr, buf.length,
                         PROT_READ | PROT_WRITE, MAP_SHARED,
                         m_fd, buf.m.offset);
        if (p == MAP_FAILED)
        {
            outError = std::string("mmap[") + std::to_string(i) +
                       "] failed: " + std::strerror(errno);
            return false;
        }
        m_buffers[i].data   = static_cast<uint8_t *>(p);
        m_buffers[i].length = buf.length;
    }
    return true;
}

void VirtualCameraOutput::unmapBuffers()
{
    for (auto &b : m_buffers)
    {
        if (b.data && b.length) ::munmap(b.data, b.length);
        b.data   = nullptr;
        b.length = 0;
    }
    m_buffers.clear();
}

// ---- sws conversion -----------------------------------------------

bool VirtualCameraOutput::ensureSws(uint32_t srcW, uint32_t srcH)
{
    // Rebuild only on size change (or first call). Conversion
    // pipeline is RGBA → (output format). sws handles both
    // colour-conversion and rescaling in one pass.
    if (m_sws && m_swsSrcW == srcW && m_swsSrcH == srcH) return true;
    freeSws();
    m_sws = sws_getContext(
        static_cast<int>(srcW), static_cast<int>(srcH), AV_PIX_FMT_RGBA,
        static_cast<int>(m_outWidth), static_cast<int>(m_outHeight),
        toAvPixFmt(m_outFormat),
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_sws)
    {
        setError("sws_getContext failed");
        return false;
    }
    m_swsSrcW = srcW;
    m_swsSrcH = srcH;
    // Resize the scratch to the OUTPUT framesize; the converter
    // writes there before we memcpy into the mmap'd kernel buffer.
    m_convertScratch.resize(
        static_cast<size_t>(m_outWidth) * m_outHeight *
        bytesPerPixel(m_outFormat));
    return true;
}

void VirtualCameraOutput::freeSws()
{
    if (m_sws)
    {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }
    m_swsSrcW = 0;
    m_swsSrcH = 0;
}

// ---- Public lifecycle ----------------------------------------------

bool VirtualCameraOutput::start(const std::string &devicePath,
                                 uint32_t width, uint32_t height,
                                 uint32_t fps, PixelFormat format,
                                 std::string &outError)
{
    // Idempotent on identical params — saves a stop/start churn
    // when syncVirtualCamera fires every frame.
    if (m_running.load() &&
        m_devicePath == devicePath &&
        m_outWidth   == width &&
        m_outHeight  == height &&
        m_outFps     == fps &&
        m_outFormat  == format)
    {
        return true;
    }
    stop();

    m_fd = ::open(devicePath.c_str(), O_RDWR | O_NONBLOCK);
    if (m_fd < 0)
    {
        outError = std::string("open(") + devicePath + ") failed: " +
                   std::strerror(errno);
        setError(outError);
        return false;
    }

    if (!negotiateFormat(width, height, format, outError))
    {
        setError(outError);
        stop();
        return false;
    }
    if (!requestAndMapBuffers(outError))
    {
        setError(outError);
        stop();
        return false;
    }

    // v4l2loopback technically accepts STREAMON even with no
    // buffers queued, but standard V4L2 requires at least one
    // queued QBUF before STREAMON. Pre-queue all empty buffers
    // so the loop is symmetric (DQBUF + fill + QBUF every push).
    for (uint32_t i = 0; i < m_buffers.size(); ++i)
    {
        v4l2_buffer buf{};
        buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        buf.bytesused = static_cast<uint32_t>(m_buffers[i].length);
        if (xioctl(m_fd, VIDIOC_QBUF, &buf) < 0)
        {
            outError = std::string("initial VIDIOC_QBUF[") +
                       std::to_string(i) + "] failed: " +
                       std::strerror(errno);
            setError(outError);
            stop();
            return false;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (xioctl(m_fd, VIDIOC_STREAMON, &type) < 0)
    {
        outError = std::string("VIDIOC_STREAMON failed: ") +
                   std::strerror(errno);
        setError(outError);
        stop();
        return false;
    }

    m_devicePath = devicePath;
    m_outFps     = fps;
    m_running.store(true);
    {
        std::lock_guard<std::mutex> lk(m_errMu);
        m_lastError.clear();
    }
    LOG_INFO("VirtualCameraOutput: streaming " +
             std::to_string(m_outWidth) + "x" +
             std::to_string(m_outHeight) + " " +
             (format == PixelFormat::YUYV ? "YUYV" : "RGB24") +
             " to " + devicePath);
    return true;
}

void VirtualCameraOutput::stop()
{
    if (m_fd >= 0)
    {
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        // Best-effort STREAMOFF; ignore failure (device might be
        // gone, e.g. user unloaded the kernel module).
        (void)xioctl(m_fd, VIDIOC_STREAMOFF, &type);
        unmapBuffers();
        ::close(m_fd);
        m_fd = -1;
    }
    freeSws();
    m_convertScratch.clear();
    m_running.store(false);
    m_devicePath.clear();
    m_outWidth = m_outHeight = m_outFps = 0;
}

// ---- Per-frame push ------------------------------------------------

bool VirtualCameraOutput::pushFrame(const uint8_t *rgba,
                                     uint32_t       rgbaWidth,
                                     uint32_t       rgbaHeight)
{
    if (!m_running.load() || !rgba) return false;

    if (!ensureSws(rgbaWidth, rgbaHeight)) return false;

    // Convert + rescale RGBA → output format in one pass.
    const uint8_t *srcSlice[1] = { rgba };
    int            srcStride[1]= { static_cast<int>(rgbaWidth) * 4 };
    uint8_t       *dstSlice[1] = { m_convertScratch.data() };
    int            dstStride[1]= {
        static_cast<int>(m_outWidth) * static_cast<int>(bytesPerPixel(m_outFormat)) };
    sws_scale(m_sws, srcSlice, srcStride, 0,
              static_cast<int>(rgbaHeight), dstSlice, dstStride);

    // DQBUF the next available output buffer; if none is ready
    // (consumer is slow) drop the frame — better latency than
    // backpressuring the render loop. EAGAIN is the expected
    // "no buffer ready" return on non-blocking fd.
    v4l2_buffer buf{};
    buf.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(m_fd, VIDIOC_DQBUF, &buf) < 0)
    {
        if (errno == EAGAIN) return true; // drop, not an error
        setError(std::string("VIDIOC_DQBUF failed: ") +
                 std::strerror(errno));
        return false;
    }

    // Copy the converted frame into the kernel-owned buffer.
    // bytesused tells the driver how much of the buffer is
    // populated — we always fill it completely.
    const size_t copySize = std::min(static_cast<size_t>(buf.length),
                                     m_convertScratch.size());
    std::memcpy(m_buffers[buf.index].data,
                m_convertScratch.data(), copySize);
    buf.bytesused = static_cast<uint32_t>(copySize);

    if (xioctl(m_fd, VIDIOC_QBUF, &buf) < 0)
    {
        setError(std::string("VIDIOC_QBUF failed: ") +
                 std::strerror(errno));
        return false;
    }
    return true;
}
