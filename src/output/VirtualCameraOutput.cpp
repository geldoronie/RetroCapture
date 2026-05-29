#include "VirtualCameraOutput.h"

#include "../utils/Logger.h"

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
// /dev nodes V4L2 might use. We only look at /dev/video*; v4l2loopback
// devices register here too.
const char *kDevDir       = "/dev";
const char *kDevPrefix    = "video";
// Driver string v4l2loopback reports in cap.driver. Older module
// builds spell it "v4l2 loopback" (with space); the current
// upstream uses "v4l2loopback" (no space). Substring-match on
// "loopback" alone — there's no other video driver in mainline
// that uses that token, so the loose match is safe.
const char *kLoopbackDrv  = "loopback";

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

        // Open O_RDONLY (not O_RDWR) for the scan: QUERYCAP needs
        // no write access, and an O_RDWR open against a device
        // some other app (OBS, a browser preview) is already
        // consuming as CAPTURE fails with EBUSY under v4l2loopback's
        // exclusive_caps mode. We'd then skip the device silently
        // and the UI would render "no device" while the device is
        // visibly present to every other app on the system. Read-
        // only open lets multiple scanners coexist.
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        v4l2_capability cap{};
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
        {
            const std::string driver(
                reinterpret_cast<const char *>(cap.driver));
            // v4l2loopback with exclusive_caps=1 narrows BOTH
            // cap.capabilities AND cap.device_caps to a single role
            // based on the open mode — bits alone don't tell us
            // "this is a loopback". The driver name does:
            // v4l2loopback is the only mainline kernel video driver
            // that uses "loopback" in its name (vivid + akvcam use
            // different strings). If it's a loopback node, it
            // always supports output by construction.
            if (driver.find(kLoopbackDrv) != std::string::npos)
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

bool VirtualCameraOutput::ensureSws(uint32_t srcW, uint32_t srcH,
                                     SourceFormat srcFmt)
{
    // Rebuild on size OR source-format change. m_swsSrcW=0 marks
    // "no context yet". Conversion handles both colour-conversion
    // and rescaling in one pass.
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
        static_cast<int>(m_outWidth), static_cast<int>(m_outHeight),
        toAvPixFmt(m_outFormat),
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_sws)
    {
        setError("sws_getContext failed");
        return false;
    }
    m_swsSrcW     = srcW;
    m_swsSrcH     = srcH;
    m_swsSrcAvFmt = static_cast<int>(avSrc);
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

bool VirtualCameraOutput::pushFrame(const uint8_t *pixels,
                                     uint32_t       srcWidth,
                                     uint32_t       srcHeight,
                                     SourceFormat   srcFormat)
{
    if (!m_running.load() || !pixels) return false;

    if (!ensureSws(srcWidth, srcHeight, srcFormat)) return false;

    // Convert + rescale source → output format in one pass.
    const int srcStridePx = (srcFormat == SourceFormat::RGBA) ? 4 : 3;
    const uint8_t *srcSlice[1] = { pixels };
    int            srcStride[1]= { static_cast<int>(srcWidth) * srcStridePx };
    uint8_t       *dstSlice[1] = { m_convertScratch.data() };
    int            dstStride[1]= {
        static_cast<int>(m_outWidth) * static_cast<int>(bytesPerPixel(m_outFormat)) };
    sws_scale(m_sws, srcSlice, srcStride, 0,
              static_cast<int>(srcHeight), dstSlice, dstStride);

    // DQBUF the next available output buffer; if none is ready
    // (consumer hasn't read the last one yet, or there isn't a
    // consumer connected at all) drop the frame — better latency
    // than backpressuring the render loop. EAGAIN is the expected
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

// ---- Module load/unload via pkexec --------------------------------

bool VirtualCameraOutput::pkexecAvailable()
{
    // pkexec on every reasonable Linux desktop lives at
    // /usr/bin/pkexec — the polkit package owns it. We don't fall
    // back to $PATH because if it isn't there, the desktop doesn't
    // ship a graphical auth agent either and the UX wouldn't work.
    return ::access("/usr/bin/pkexec", X_OK) == 0;
}

namespace
{
// Forks pkexec with the given argv (argv[0] must be "pkexec"; we
// don't free its descendants explicitly because polkit reaps them
// when the auth dialog closes). Captures combined stdout+stderr
// via a pipe. Returns the child's exit status (or -1 on a setup
// failure with errno preserved). Output is appended to `outText`.
//
// Why fork+exec instead of system(): we want both the exit code
// AND the textual error message (e.g. "ERROR: could not insert
// 'v4l2loopback': Operation not permitted"). system() merges
// stderr into the shell's stderr; we'd lose it.
int runPkexec(const std::vector<std::string> &argv, std::string &outText)
{
    int pipefd[2] = {-1, -1};
    if (::pipe(pipefd) != 0)
    {
        outText = std::string("pipe() failed: ") + std::strerror(errno);
        return -1;
    }
    const pid_t pid = ::fork();
    if (pid < 0)
    {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        outText = std::string("fork() failed: ") + std::strerror(errno);
        return -1;
    }
    if (pid == 0)
    {
        // child — redirect stdout + stderr to write end of pipe
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        // Build a NULL-terminated argv. We're about to be replaced
        // by exec so heap leaks here are harmless.
        std::vector<char *> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto &s : argv) cargv.push_back(const_cast<char *>(s.c_str()));
        cargv.push_back(nullptr);
        ::execvp(cargv[0], cargv.data());
        // execvp only returns on failure.
        std::fprintf(stderr, "execvp(%s) failed: %s\n",
                     cargv[0], std::strerror(errno));
        ::_exit(127);
    }
    // parent — close write end, read until EOF, wait for child.
    ::close(pipefd[1]);
    char buf[1024];
    for (;;)
    {
        const ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        if (n > 0) outText.append(buf, static_cast<size_t>(n));
        else if (n == 0) break;
        else if (errno != EINTR) break;
    }
    ::close(pipefd[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) { /* retry */ }
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

// Trim trailing whitespace + collapse multiple blank lines so the
// UI surface stays compact.
std::string tidy(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

// Strict allowlist for the cardLabel passed into modprobe — letters,
// digits, space, dash, underscore. modprobe's KV parser accepts
// arbitrary text for card_label but we keep it boring so the
// downstream consumer UIs render predictably AND so there's never
// any shell-escape concern about what we pass to execvp (which
// doesn't go through a shell, but defence in depth).
std::string sanitizeCardLabel(const std::string &in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == ' ' || c == '-' || c == '_')
            out.push_back(c);
    }
    if (out.empty()) out = "RetroCapture";
    return out;
}
} // namespace

VirtualCameraOutput::ModuleOpResult
VirtualCameraOutput::loadV4l2LoopbackModule(const std::string &cardLabel)
{
    ModuleOpResult r;
    if (!pkexecAvailable())
    {
        r.message = "pkexec not found at /usr/bin/pkexec. Install "
                    "the polkit package or run the modprobe command "
                    "manually in a terminal.";
        r.exitCode = 127;
        return r;
    }
    const std::string label = sanitizeCardLabel(cardLabel);
    // execvp argv: ["pkexec", "modprobe", "v4l2loopback",
    //               "exclusive_caps=1", "card_label=<label>"]
    std::string text;
    const int   rc = runPkexec(
        {"pkexec", "modprobe", "v4l2loopback",
         "exclusive_caps=1",
         std::string("card_label=") + label},
        text);
    r.exitCode = rc;
    r.ok       = (rc == 0);
    if (r.ok)
    {
        r.message = "Module loaded.";
    }
    else
    {
        // pkexec uses 126 for "user cancelled the auth dialog";
        // surface a friendly note instead of dumping pkexec's
        // own English message.
        if (rc == 126)
            r.message = "Authentication cancelled.";
        else if (rc == 127)
            r.message = "pkexec failed to launch.";
        else
            r.message = tidy(text);
        if (r.message.empty())
            r.message = "modprobe failed with exit code " +
                        std::to_string(rc) + ".";
    }
    return r;
}

VirtualCameraOutput::ModuleOpResult
VirtualCameraOutput::unloadV4l2LoopbackModule()
{
    ModuleOpResult r;
    if (!pkexecAvailable())
    {
        r.message = "pkexec not found at /usr/bin/pkexec.";
        r.exitCode = 127;
        return r;
    }
    std::string text;
    // -f forces removal but only succeeds if no process still
    // holds a file descriptor on a loopback device. We DON'T
    // pass -f — the error path that surfaces "Resource busy"
    // is informative ("close OBS / Chrome and try again").
    const int rc = runPkexec({"pkexec", "rmmod", "v4l2loopback"}, text);
    r.exitCode = rc;
    r.ok       = (rc == 0);
    if (r.ok)
    {
        r.message = "Module removed.";
    }
    else
    {
        if (rc == 126)
            r.message = "Authentication cancelled.";
        else if (rc == 127)
            r.message = "pkexec failed to launch.";
        else
            r.message = tidy(text);
        if (r.message.empty())
            r.message = "rmmod failed with exit code " +
                        std::to_string(rc) + ".";
    }
    return r;
}
