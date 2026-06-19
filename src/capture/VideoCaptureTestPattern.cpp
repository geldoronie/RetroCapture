#include "VideoCaptureTestPattern.h"
#include "../utils/Logger.h"

#include <cstring>

VideoCaptureTestPattern::VideoCaptureTestPattern() = default;
VideoCaptureTestPattern::~VideoCaptureTestPattern() { close(); }

bool VideoCaptureTestPattern::open(const std::string & /*device*/)
{
    m_open = true;
    if (m_buffer.empty())
        m_buffer.resize(static_cast<size_t>(m_width) * m_height * 3, 0);
    LOG_INFO("VideoCaptureTestPattern opened: " + std::to_string(m_width) + "x" +
             std::to_string(m_height) + " @ " + std::to_string(m_fps) + "fps (RGB24)");
    return true;
}

void VideoCaptureTestPattern::close()
{
    m_capturing = false;
    m_open = false;
}

bool VideoCaptureTestPattern::setFormat(uint32_t width, uint32_t height, uint32_t /*pixelFormat*/)
{
    if (width > 0) m_width = width;
    if (height > 0) m_height = height;
    m_buffer.assign(static_cast<size_t>(m_width) * m_height * 3, 0);
    return true;
}

bool VideoCaptureTestPattern::setFramerate(uint32_t fps)
{
    if (fps > 0) m_fps = fps;
    return true;
}

bool VideoCaptureTestPattern::startCapture()
{
    if (!m_open) open("");
    m_capturing = true;
    return true;
}

void VideoCaptureTestPattern::stopCapture()
{
    m_capturing = false;
}

std::vector<DeviceInfo> VideoCaptureTestPattern::listDevices()
{
    return {DeviceInfo{"test", "Test Pattern", "synthetic", true}};
}

void VideoCaptureTestPattern::renderPattern()
{
    const size_t needed = static_cast<size_t>(m_width) * m_height * 3;
    if (m_buffer.size() < needed)
        m_buffer.resize(needed);

    // 8 SMPTE-style vertical colour bars (R,G,B per bar). Having all three
    // channels vary across the frame is what lets the smoke-test assert the
    // colour path didn't collapse to grayscale or swap channels.
    static const uint8_t bars[8][3] = {
        {255, 255, 255}, // white
        {255, 255, 0},   // yellow
        {0, 255, 255},   // cyan
        {0, 255, 0},     // green
        {255, 0, 255},   // magenta
        {255, 0, 0},     // red
        {0, 0, 255},     // blue
        {16, 16, 16},    // near-black
    };

    const uint64_t f = m_frameCounter.fetch_add(1, std::memory_order_relaxed);
    const uint32_t barW = (m_width > 0) ? (m_width / 8) : 1;
    // A moving block sweeps left→right one column per frame, giving temporal
    // variance so a frozen/duplicated frame is detectable downstream.
    const uint32_t markerX = static_cast<uint32_t>(f % (m_width ? m_width : 1));
    const uint32_t markerH = m_height / 8;

    for (uint32_t y = 0; y < m_height; ++y)
    {
        uint8_t *row = m_buffer.data() + static_cast<size_t>(y) * m_width * 3;
        for (uint32_t x = 0; x < m_width; ++x)
        {
            uint32_t bar = barW ? (x / barW) : 0;
            if (bar > 7) bar = 7;
            uint8_t r = bars[bar][0], g = bars[bar][1], b = bars[bar][2];
            // Moving marker: a black vertical strip in a band near the top.
            if (y < markerH && (x >= markerX && x < markerX + 8))
            {
                r = g = b = 0;
            }
            uint8_t *px = row + static_cast<size_t>(x) * 3;
            px[0] = r;
            px[1] = g;
            px[2] = b;
        }
    }
}

bool VideoCaptureTestPattern::captureFrame(Frame &frame)
{
    if (!m_open)
        return false;
    renderPattern();
    frame.data = m_buffer.data();
    frame.size = static_cast<size_t>(m_width) * m_height * 3;
    frame.width = m_width;
    frame.height = m_height;
    frame.format = 0; // RGB24 → FrameProcessor uploads GL_RGB directly
    return true;
}
