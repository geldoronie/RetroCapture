#include "VideoCaptureScreen.h"
#include "../utils/Logger.h"

#include <cstring>

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
    frame.format = m_pixelFormat; // 0 == RGB24
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

    // Source byte indices for R/G/B inside each 4-byte pixel.
    int rIdx, gIdx, bIdx;
    switch (format)
    {
        case ScreenPixelFormat::BGRA:
        case ScreenPixelFormat::BGRX: rIdx = 2; gIdx = 1; bIdx = 0; break;
        case ScreenPixelFormat::RGBA:
        case ScreenPixelFormat::RGBX:
        default:                      rIdx = 0; gIdx = 1; bIdx = 2; break;
    }

    std::lock_guard<std::mutex> lock(m_frameMutex);
    m_frameBuf.resize(static_cast<size_t>(rw) * rh * 3);
    uint8_t *dst = m_frameBuf.data();
    for (uint32_t y = 0; y < rh; ++y)
    {
        const uint8_t *srow = data + static_cast<size_t>(ry + y) * stride +
                              static_cast<size_t>(rx) * 4;
        uint8_t *drow = dst + static_cast<size_t>(y) * rw * 3;
        for (uint32_t x = 0; x < rw; ++x)
        {
            const uint8_t *p = srow + static_cast<size_t>(x) * 4;
            drow[x * 3 + 0] = p[rIdx];
            drow[x * 3 + 1] = p[gIdx];
            drow[x * 3 + 2] = p[bIdx];
        }
    }
    m_width.store(rw);
    m_height.store(rh);
    m_haveFrame = true;
    m_receiving.store(true);
}
