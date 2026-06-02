#include "VideoCaptureScreen.h"
#include "../utils/Logger.h"

#include <chrono>
#include <cstring>
#include <thread>

// ─────────────────────────────────────────────────────────────────────
// Cross-platform shell for the screen-capture source (#107).
//
// The real per-platform grabbers (PipeWire/portal on Linux, DXGI/WGC on
// Windows, ScreenCaptureKit on macOS) plug into Backend and push RGB24
// frames through deliverFrame(). Until a platform backend is wired in,
// this shell runs a moving test pattern so the whole source path —
// selection, format negotiation, FrameProcessor, streaming/recording —
// can be exercised end to end. Frames are RGB24 (size = w*h*3,
// pixelFormat 0) to match what the FrameProcessor already accepts from
// the DirectShow / Remote paths; backends convert their native BGRA.
// ─────────────────────────────────────────────────────────────────────

struct VideoCaptureScreen::Backend
{
    // Placeholder until the platform grabbers land. Holds the worker
    // thread that, for now, synthesises the test pattern.
    std::thread        thread;
    std::atomic<bool>  running{false};
};

VideoCaptureScreen::VideoCaptureScreen()
    : m_backend(std::make_unique<Backend>())
{
}

VideoCaptureScreen::~VideoCaptureScreen()
{
    stopCapture();
    close();
}

std::vector<DeviceInfo> VideoCaptureScreen::listDevices()
{
    // TODO(#107): real monitor/window enumeration per platform. On
    // Wayland the portal drives selection via its own dialog, so this
    // returns a single synthetic entry there; on Windows/macOS it will
    // list each display and capturable window.
    std::vector<DeviceInfo> out;
    DeviceInfo d;
    d.id        = "monitor:0";
    d.name      = "Primary display";
    d.driver    = "screen";
    d.available = true;
    out.push_back(d);
    return out;
}

bool VideoCaptureScreen::open(const std::string &device)
{
    m_target = device;
    LOG_INFO("VideoCaptureScreen: open target '" +
             (device.empty() ? std::string("(default)") : device) + "'");
    // The shell test pattern has no real target to validate; platform
    // backends will fail here when the target can't be acquired.
    m_open.store(true);
    return true;
}

void VideoCaptureScreen::close()
{
    if (!m_open.exchange(false)) return;
    LOG_INFO("VideoCaptureScreen: close");
    m_width.store(0);
    m_height.store(0);
    m_receiving.store(false);
}

bool VideoCaptureScreen::setFormat(uint32_t width, uint32_t height, uint32_t)
{
    // The desktop dictates its own resolution; we honour the crop region
    // instead. Seed a sane default so a consumer that queries before the
    // first frame doesn't see 0x0.
    if (m_width.load() == 0 && width)  m_width.store(width);
    if (m_height.load() == 0 && height) m_height.store(height);
    return true;
}

bool VideoCaptureScreen::setFramerate(uint32_t) { return true; }

void VideoCaptureScreen::setRegion(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    std::lock_guard<std::mutex> lock(m_regionMutex);
    m_regionX = x; m_regionY = y; m_regionW = w; m_regionH = h;
}

bool VideoCaptureScreen::isReceivingFrames() const
{
    return m_open.load() && m_receiving.load();
}

bool VideoCaptureScreen::startCapture()
{
    if (!m_open.load())
    {
        LOG_ERROR("VideoCaptureScreen::startCapture — not open");
        return false;
    }
    if (m_backend->running.load()) return true;
    m_backend->running.store(true);

    // Placeholder generator thread. Replaced per-platform; kept here so
    // the integration is exercisable before the grabbers land.
    m_backend->thread = std::thread([this]() {
        const uint32_t w = 1280, h = 720;
        m_width.store(w);
        m_height.store(h);
        m_pixelFormat = 0; // RGB24
        uint32_t tick = 0;
        while (m_backend->running.load())
        {
            // A slow diagonal gradient so it's visibly "live".
            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                m_frameBuf.resize(static_cast<size_t>(w) * h * 3);
                uint8_t *p = m_frameBuf.data();
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        const size_t i = (static_cast<size_t>(y) * w + x) * 3;
                        p[i + 0] = static_cast<uint8_t>((x + tick) & 0xFF);
                        p[i + 1] = static_cast<uint8_t>((y + tick) & 0xFF);
                        p[i + 2] = static_cast<uint8_t>((x + y) & 0xFF);
                    }
                m_haveFrame = true;
            }
            m_receiving.store(true);
            ++tick;
            std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 fps
        }
    });
    return true;
}

void VideoCaptureScreen::stopCapture()
{
    if (!m_backend) return;
    if (!m_backend->running.exchange(false)) return;
    if (m_backend->thread.joinable()) m_backend->thread.join();
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
    frame.format = m_pixelFormat;
    return true;
}
