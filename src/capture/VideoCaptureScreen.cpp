#include "VideoCaptureScreen.h"
#include "../utils/Logger.h"

#include <cstring>

#ifdef RETROCAPTURE_SCREEN_DMABUF
#include "../renderer/glad_loader.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <unistd.h>
#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL
#endif
#endif

// ─────────────────────────────────────────────────────────────────────
// Cross-platform glue for the screen-capture source (#107). The actual
// grabbing lives in the platform ScreenBackend (createScreenBackend);
// this class drives it through the IVideoCapture contract and turns the
// backend's packed-32-bit frames into the cropped RGB24 the rest of the
// pipeline consumes.
// ─────────────────────────────────────────────────────────────────────

VideoCaptureScreen::VideoCaptureScreen()
    : m_backend(createScreenBackend(*this))
{
}

VideoCaptureScreen::~VideoCaptureScreen()
{
    stopCapture();
    close();
}

std::vector<DeviceInfo> VideoCaptureScreen::listDevices()
{
    return m_backend ? m_backend->listTargets() : std::vector<DeviceInfo>{};
}

bool VideoCaptureScreen::open(const std::string &device)
{
    m_target = device;
    LOG_INFO("VideoCaptureScreen: open target '" +
             (device.empty() ? std::string("(default)") : device) + "'");
    m_open.store(true);
    return true;
}

void VideoCaptureScreen::close()
{
    if (!m_open.exchange(false)) return;
    stopCapture();
    LOG_INFO("VideoCaptureScreen: close");
    m_width.store(0);
    m_height.store(0);
    m_receiving.store(false);
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_haveFrame = false;
        m_frameBuf.clear();
    }
}

bool VideoCaptureScreen::setFormat(uint32_t width, uint32_t height, uint32_t)
{
    // The desktop dictates its own resolution; the crop region is the
    // only sizing knob. Seed a default so a query before the first frame
    // doesn't see 0x0.
    if (m_width.load() == 0 && width)   m_width.store(width);
    if (m_height.load() == 0 && height) m_height.store(height);
    return true;
}

bool VideoCaptureScreen::setFramerate(uint32_t) { return true; }

void VideoCaptureScreen::setRegion(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    std::lock_guard<std::mutex> lock(m_regionMutex);
    m_regionX = x; m_regionY = y; m_regionW = w; m_regionH = h;
}

bool VideoCaptureScreen::startCapture()
{
    if (!m_open.load())
    {
        LOG_ERROR("VideoCaptureScreen::startCapture — not open");
        return false;
    }
    if (!m_backend) return false;
    return m_backend->start(m_target, m_captureCursor.load());
}

void VideoCaptureScreen::stopCapture()
{
    if (m_backend) m_backend->stop();
    m_receiving.store(false);
}

bool VideoCaptureScreen::captureFrame(Frame &frame)
{
    return captureLatestFrame(frame);
}

bool VideoCaptureScreen::captureLatestFrame(Frame &frame)
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (!m_haveFrame || m_frameBuf.empty()) return false;
    frame.data   = m_frameBuf.data();
    frame.size   = m_frameBuf.size();
    frame.width  = m_width.load();
    frame.height = m_height.load();
    frame.format = m_pixelFormat.load();
    return true;
}

void VideoCaptureScreen::onScreenFrame(const uint8_t *data, uint32_t w, uint32_t h,
                                       uint32_t stride, ScreenPixelFormat format)
{
    if (!data || w == 0 || h == 0) return;

    // Resolve the crop region against the live frame size.
    uint32_t rx, ry, rw, rh;
    {
        std::lock_guard<std::mutex> lock(m_regionMutex);
        rx = m_regionX; ry = m_regionY; rw = m_regionW; rh = m_regionH;
    }
    if (rw == 0 || rh == 0) { rx = 0; ry = 0; rw = w; rh = h; } // full target
    if (rx >= w) rx = 0;
    if (ry >= h) ry = 0;
    if (rx + rw > w) rw = w - rx;
    if (ry + rh > h) rh = h - ry;
    if (rw == 0 || rh == 0) return;

    // Keep the packed 32-bit data as-is — no per-pixel colour conversion
    // here (it ran on the PipeWire process callback and throttled the
    // whole stream on large monitors). The GPU does the BGRA/RGBA→RGB
    // swizzle on upload (FrameProcessor). We only copy out the cropped
    // sub-rect, row by row.
    const uint32_t outFmt = (format == ScreenPixelFormat::RGBA ||
                             format == ScreenPixelFormat::RGBX)
                                ? RC_PIXFMT_RGBA
                                : RC_PIXFMT_BGRA;

    std::lock_guard<std::mutex> lock(m_frameMutex);
    const size_t dstStride = static_cast<size_t>(rw) * 4;
    m_frameBuf.resize(dstStride * rh);
    uint8_t *dst = m_frameBuf.data();
    for (uint32_t y = 0; y < rh; ++y)
    {
        const uint8_t *srow = data + static_cast<size_t>(ry + y) * stride +
                              static_cast<size_t>(rx) * 4;
        std::memcpy(dst + static_cast<size_t>(y) * dstStride, srow, dstStride);
    }
    m_pixelFormat.store(outFmt);
    m_width.store(rw);
    m_height.store(rh);
    m_haveFrame = true;
    m_receiving.store(true);
}

void VideoCaptureScreen::onScreenDmabuf(const ScreenDmabufFrame &frame)
{
#ifdef RETROCAPTURE_SCREEN_DMABUF
    std::lock_guard<std::mutex> lock(m_dmabufMutex);
    if (m_pendingDmabuf.fd >= 0) ::close(m_pendingDmabuf.fd); // drop unconsumed
    m_pendingDmabuf = frame;        // we now own frame.fd
    m_width.store(frame.width);
    m_height.store(frame.height);
    m_receiving.store(true);
#else
    // No DMABUF build → backend never produces these; nothing to own.
    (void)frame;
#endif
}

unsigned int VideoCaptureScreen::getGpuTexture(uint32_t &w, uint32_t &h)
{
#ifdef RETROCAPTURE_SCREEN_DMABUF
    ScreenDmabufFrame f;
    {
        std::lock_guard<std::mutex> lock(m_dmabufMutex);
        if (m_pendingDmabuf.fd < 0)
        {
            if (m_glTex) { w = m_glW; h = m_glH; } // reuse last imported
            return m_glTex;
        }
        f = m_pendingDmabuf;
        m_pendingDmabuf.fd = -1; // consumed
    }

    static auto pCreate =
        reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    static auto pDestroy =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    typedef void (*RC_TargetTex)(GLenum, void *);
    static auto pTarget =
        reinterpret_cast<RC_TargetTex>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));

    EGLDisplay dpy = eglGetCurrentDisplay();
    if (!pCreate || !pTarget || dpy == EGL_NO_DISPLAY)
    {
        if (f.fd >= 0) ::close(f.fd);
        static bool warned = false;
        if (!warned) { warned = true; LOG_WARN("VideoCaptureScreen: EGL DMABUF import unavailable — falling back"); }
        return 0;
    }

    EGLint attribs[40];
    int n = 0;
    attribs[n++] = EGL_WIDTH;                     attribs[n++] = static_cast<EGLint>(f.width);
    attribs[n++] = EGL_HEIGHT;                    attribs[n++] = static_cast<EGLint>(f.height);
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;      attribs[n++] = static_cast<EGLint>(f.drmFourcc);
    attribs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;     attribs[n++] = f.fd;
    attribs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attribs[n++] = static_cast<EGLint>(f.offset);
    attribs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  attribs[n++] = static_cast<EGLint>(f.stride);
    if (f.hasModifier)
    {
        attribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attribs[n++] = static_cast<EGLint>(f.modifier & 0xFFFFFFFFu);
        attribs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attribs[n++] = static_cast<EGLint>(f.modifier >> 32);
    }
    attribs[n++] = EGL_NONE;

    EGLImageKHR img = pCreate(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                              static_cast<EGLClientBuffer>(nullptr), attribs);
    ::close(f.fd); // EGL holds its own reference to the buffer
    if (img == EGL_NO_IMAGE_KHR)
    {
        static bool warned = false;
        if (!warned) { warned = true; LOG_WARN("VideoCaptureScreen: eglCreateImage(DMABUF) failed — falling back"); }
        return 0;
    }

    if (m_glTex == 0)
    {
        glGenTextures(1, &m_glTex);
        glBindTexture(GL_TEXTURE_2D, m_glTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    }
    glBindTexture(GL_TEXTURE_2D, m_glTex);
    pTarget(GL_TEXTURE_2D, img);

    // Bind first, then drop the previous image (the texture now samples
    // the new one).
    if (m_eglImage && pDestroy) pDestroy(dpy, static_cast<EGLImageKHR>(m_eglImage));
    m_eglImage = img;

    m_glW = f.width; m_glH = f.height;
    w = m_glW; h = m_glH;
    return m_glTex;
#else
    (void)w; (void)h;
    return 0;
#endif
}
