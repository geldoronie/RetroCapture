#include "VideoCaptureTestPattern.h"
#include "../utils/Logger.h"
#include "../utils/MediaClock.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

VideoCaptureTestPattern::VideoCaptureTestPattern() = default;
VideoCaptureTestPattern::~VideoCaptureTestPattern() { close(); }

bool VideoCaptureTestPattern::open(const std::string & /*device*/)
{
    m_open = true;
    const char *variant = std::getenv("RETROCAPTURE_TEST_PATTERN");
    const std::string v = variant ? variant : "";
    m_variant = v == "levels"       ? Variant::Levels
                : v == "interlaced" ? Variant::Interlaced
                : v == "motion"     ? Variant::Motion
                : v == "edges"      ? Variant::Edges
                                    : Variant::Bars;
    if (m_variant == Variant::Levels)
        LOG_INFO("VideoCaptureTestPattern: 'levels' variant (grey steps + colour bars)");
    else if (m_variant == Variant::Edges)
        LOG_INFO("VideoCaptureTestPattern: 'edges' variant (static rings / checkerboard / starburst)");
    else if (m_variant == Variant::Interlaced)
        LOG_INFO("VideoCaptureTestPattern: 'interlaced' variant (woven TFF fields, moving bar)");
    else if (m_variant == Variant::Motion)
        LOG_INFO("VideoCaptureTestPattern: 'motion' variant (scrolling background, sprites, static HUD)");
    if (const char *cs = std::getenv("RETROCAPTURE_TEST_CUT"))
        m_cutPeriod = std::atof(cs);
    if (const char *ps = std::getenv("RETROCAPTURE_TEST_PAUSE"))
        std::sscanf(ps, "%lf,%lf", &m_pauseRun, &m_pauseStill);
    if (const char *g = std::getenv("RETROCAPTURE_TEST_GAME_FPS"))
    {
        m_gameFps = std::atof(g);
        if (m_variant == Variant::Motion && m_gameFps > 0.0)
            LOG_INFO("VideoCaptureTestPattern: new picture at " + std::to_string(m_gameFps) +
                     " fps (repeated captures in between)");
    }
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
    // RETROCAPTURE_TEST_NATIVE=WxH: always deliver this size, whatever is
    // asked (a grabber that only offers some modes), so the pipeline's
    // logical-size downscale can be tested.
    if (const char *n = std::getenv("RETROCAPTURE_TEST_NATIVE"))
    {
        uint32_t nw = 0, nh = 0;
        if (std::sscanf(n, "%ux%u", &nw, &nh) == 2 && nw > 0 && nh > 0)
        {
            m_width = nw;
            m_height = nh;
        }
    }
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

void VideoCaptureTestPattern::renderLevels()
{
    static const uint8_t steps[8] = {0, 16, 32, 64, 128, 192, 235, 255};
    static const uint8_t bars[8][3] = {
        {255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
        {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {16, 16, 16},
    };
    const uint32_t colW = (m_width > 0) ? (m_width / 8) : 1;
    for (uint32_t y = 0; y < m_height; ++y)
    {
        uint8_t *row = m_buffer.data() + static_cast<size_t>(y) * m_width * 3;
        for (uint32_t x = 0; x < m_width; ++x)
        {
            uint32_t c = colW ? (x / colW) : 0;
            if (c > 7) c = 7;
            uint8_t *px = row + static_cast<size_t>(x) * 3;
            if (y < m_height / 2)
            {
                px[0] = px[1] = px[2] = steps[c];
            }
            else
            {
                px[0] = bars[c][0];
                px[1] = bars[c][1];
                px[2] = bars[c][2];
            }
        }
    }
}

void VideoCaptureTestPattern::renderEdges()
{
    renderEdgesTo(m_buffer.data(), m_width, m_height);
}

void VideoCaptureTestPattern::renderEdgesTo(uint8_t *rgb, uint32_t width, uint32_t height)
{
    // Static and resolution-independent: every pixel samples the same
    // continuous pattern at its centre, so a WxH render is the ground truth
    // for an upscale of a smaller render (#251).
    const double W = width ? width : 1, H = height ? height : 1;
    const double aspect = W / H;
    const double kPi = 3.14159265358979323846;
    const double ca = std::cos(15.0 * kPi / 180.0), sa = std::sin(15.0 * kPi / 180.0);
    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t *row = rgb + static_cast<size_t>(y) * width * 3;
        const double v = (y + 0.5) / H;
        for (uint32_t x = 0; x < width; ++x)
        {
            const double u = (x + 0.5) / W;
            const double px = u * aspect, py = v; // height units
            uint8_t r, g, b;
            if (u < 0.5)
            {
                // Concentric rings.
                const double dx = px - 0.25 * aspect, dy = py - 0.5;
                const bool on = (static_cast<long>(std::floor(std::sqrt(dx * dx + dy * dy) * 20.0)) & 1) != 0;
                r = g = b = on ? 230 : 20;
            }
            else if (v < 0.5)
            {
                // Checkerboard rotated by 15 degrees.
                const double qx = (px * ca - py * sa) * 12.0, qy = (px * sa + py * ca) * 12.0;
                const bool on = ((static_cast<long>(std::floor(qx)) + static_cast<long>(std::floor(qy))) & 1) != 0;
                r = on ? 240 : 30;
                g = on ? 240 : 30;
                b = on ? 200 : 60;
            }
            else
            {
                // Colour starburst, 24 sectors.
                const double a = std::atan2(py - 0.75, px - 0.75 * aspect);
                const bool on = (static_cast<long>(std::floor((a + kPi) / (2.0 * kPi) * 24.0)) & 1) != 0;
                r = on ? 220 : 30;
                g = on ? 70 : 90;
                b = on ? 40 : 210;
            }
            uint8_t *p = row + static_cast<size_t>(x) * 3;
            p[0] = r;
            p[1] = g;
            p[2] = b;
        }
    }
}

void VideoCaptureTestPattern::renderInterlaced(uint64_t f)
{
    const uint32_t barW = 64;
    const uint32_t step = 16; // px per field
    const uint32_t span = m_width > barW ? m_width - barW : 1;
    for (uint32_t y = 0; y < m_height; ++y)
    {
        // Even rows = top field (first in time), odd rows = bottom field.
        const uint64_t fieldIndex = 2 * f + (y & 1u);
        const uint32_t x0 = static_cast<uint32_t>((fieldIndex * step) % span);
        uint8_t *row = m_buffer.data() + static_cast<size_t>(y) * m_width * 3;
        std::memset(row, 0, static_cast<size_t>(m_width) * 3);
        std::memset(row + static_cast<size_t>(x0) * 3, 255, static_cast<size_t>(barW) * 3);
    }
}

namespace
{
// Small integer hash -> 0..255 (deterministic background texture).
inline uint32_t hash2(int32_t x, int32_t y)
{
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (h ^ (h >> 16)) & 0xFFu;
}
inline int32_t floorDiv(int32_t a, int32_t b)
{
    return (a >= 0) ? a / b : -((-a + b - 1) / b);
}
} // namespace

void VideoCaptureTestPattern::renderMotion(uint8_t *rgb, uint32_t w, uint32_t h, double t, bool flash)
{
    if (!rgb || w == 0 || h == 0)
        return;
    // Sizes scale with the frame height (designed at 720p).
    const double k = static_cast<double>(h) / 720.0;
    const int32_t tile = static_cast<int32_t>(32 * k) > 4 ? static_cast<int32_t>(32 * k) : 4;
    const int32_t cell = tile / 8 > 1 ? tile / 8 : 1;
    // Wrap the time so the float offsets stay exact over long runs.
    const double tw = std::fmod(t, 3600.0);
    const double scrollX = tw * 240.0 * k;                  // 4 px/frame at 60 fps
    const double scrollY = 24.0 * k * std::sin(tw * 0.9);   // slow vertical sway
    const int32_t ox = static_cast<int32_t>(std::floor(scrollX));
    const int32_t oy = static_cast<int32_t>(std::floor(scrollY));

    struct Sprite { double x, y, r; uint8_t c[3]; };
    const double W = static_cast<double>(w), H = static_cast<double>(h);
    const Sprite sprites[3] = {
        {W * (0.50 + 0.36 * std::cos(tw * 1.3)), H * (0.55 + 0.22 * std::sin(tw * 2.1)), 46 * k, {230, 60, 50}},
        {W * (0.30 + 0.20 * std::sin(tw * 2.7)), H * (0.45 + 0.25 * std::cos(tw * 1.7)), 34 * k, {60, 200, 80}},
        {W * (0.72 + 0.18 * std::cos(tw * 3.3 + 1.0)), H * (0.62 + 0.15 * std::sin(tw * 2.9)), 58 * k, {250, 210, 40}},
    };
    const uint32_t hudH = h / 12;
    const uint32_t flashY0 = h - h / 8;
    const int counter = static_cast<int>(std::floor(t * 2.0)) % 10000;

    for (uint32_t y = 0; y < h; ++y)
    {
        uint8_t *row = rgb + static_cast<size_t>(y) * w * 3;
        if (y >= flashY0)
        {
            std::memset(row, flash ? 255 : 0, static_cast<size_t>(w) * 3);
            continue;
        }
        if (y < hudH)
        {
            // Static HUD: dark bar, fixed "text" blocks, a 4-digit counter.
            for (uint32_t x = 0; x < w; ++x)
            {
                uint8_t *px = row + static_cast<size_t>(x) * 3;
                px[0] = 16; px[1] = 24; px[2] = 64;
                const uint32_t gx = x / (hudH / 3 + 1), gy = y / (hudH / 3 + 1);
                if (gy == 1 && x < w / 2 && (hash2(static_cast<int32_t>(gx), 7) & 1u))
                {
                    px[0] = px[1] = px[2] = 220;
                }
                if (gy == 1 && x >= (w * 3) / 4)
                {
                    const uint32_t digit = (gx - ((w * 3) / 4) / (hudH / 3 + 1)) % 5;
                    const int val = (digit < 4) ? (counter / (digit == 0 ? 1000 : digit == 1 ? 100 : digit == 2 ? 10 : 1)) % 10 : 0;
                    if (digit < 4 && ((val + gx) & 1))
                    {
                        px[0] = 250; px[1] = 230; px[2] = 90;
                    }
                }
            }
            continue;
        }
        const int32_t v = static_cast<int32_t>(y) + oy;
        const int32_t ty = floorDiv(v, tile);
        const int32_t cy = floorDiv(v, cell);
        for (uint32_t x = 0; x < w; ++x)
        {
            const int32_t u = static_cast<int32_t>(x) + ox;
            const int32_t tx = floorDiv(u + ((ty & 1) ? tile / 2 : 0), tile);
            const int32_t cx = floorDiv(u, cell);
            // Bricks with a blocky per-cell texture and dark mortar lines.
            const uint32_t n = hash2(cx, cy);
            const uint32_t base = hash2(tx, ty);
            const int32_t lu = (u + ((ty & 1) ? tile / 2 : 0)) - tx * tile;
            const int32_t lv = v - ty * tile;
            uint8_t r, g, b;
            if (lu < cell || lv < cell)
            {
                r = 40; g = 30; b = 30;
            }
            else
            {
                r = static_cast<uint8_t>(110 + (base & 63) + (n & 31));
                g = static_cast<uint8_t>(60 + ((base >> 2) & 31) + (n & 15));
                b = static_cast<uint8_t>(50 + (n & 31));
            }
            for (const Sprite &s : sprites)
            {
                const double dx = static_cast<double>(x) + 0.5 - s.x;
                const double dy = static_cast<double>(y) + 0.5 - s.y;
                const double d2 = dx * dx + dy * dy;
                if (d2 < s.r * s.r)
                {
                    const double rIn = s.r - 4.0 * k;
                    if (d2 > rIn * rIn)
                    {
                        r = g = b = 10; // outline
                    }
                    else if (dx > -0.5 * s.r && dx < -0.1 * s.r && dy > -0.5 * s.r && dy < -0.1 * s.r)
                    {
                        r = g = b = 250; // highlight
                    }
                    else
                    {
                        r = s.c[0]; g = s.c[1]; b = s.c[2];
                    }
                }
            }
            uint8_t *px = row + static_cast<size_t>(x) * 3;
            px[0] = r;
            px[1] = g;
            px[2] = b;
        }
    }
}

void VideoCaptureTestPattern::renderPattern(uint64_t f, int64_t timestampUs)
{
    const size_t needed = static_cast<size_t>(m_width) * m_height * 3;
    if (m_buffer.size() < needed)
        m_buffer.resize(needed);
    if (m_variant == Variant::Levels)
    {
        renderLevels();
        return;
    }
    if (m_variant == Variant::Interlaced)
    {
        renderInterlaced(f);
        return;
    }
    if (m_variant == Variant::Motion)
    {
        // #256: RETROCAPTURE_TEST_GAME_FPS=N draws a new picture only N
        // times a second (a game below the capture rate), so the capture
        // repeats each one exactly, like a console's 60 Hz signal does.
        // Counted in captured frames (not timestamps), so every picture
        // repeats exactly fps / N times, as on a console's fixed refresh.
        int64_t ts = timestampUs;
        if (m_gameFps > 0.0 && m_fps > 0)
        {
            const double picture = std::floor(static_cast<double>(f) * m_gameFps / static_cast<double>(m_fps));
            ts = static_cast<int64_t>(picture / m_gameFps * 1e6);
        }
        // RETROCAPTURE_TEST_PAUSE=run,still (seconds): the picture moves for
        // `run` s, then stays still for `still` s, repeatedly (a paused game
        // or a still camera), resuming where it stopped.
        if (m_pauseRun > 0.0 && m_pauseStill > 0.0)
        {
            const double t = static_cast<double>(ts) / 1e6;
            const double period = m_pauseRun + m_pauseStill;
            const double k = std::floor(t / period);
            const double eff = k * m_pauseRun + std::min(t - k * period, m_pauseRun);
            ts = static_cast<int64_t>(eff * 1e6);
        }
        renderMotion(m_buffer.data(), m_width, m_height, static_cast<double>(ts) / 1e6,
                     (ts % 1000000LL) < 100000LL);
        // RETROCAPTURE_TEST_CUT=S: every S seconds the picture switches
        // between normal and inverted colours, a scene cut on every block.
        if (m_cutPeriod > 0.0 &&
            (static_cast<long long>(std::floor(static_cast<double>(ts) / 1e6 / m_cutPeriod)) & 1) != 0)
        {
            const size_t n = static_cast<size_t>(m_width) * m_height * 3;
            for (size_t i = 0; i < n; ++i)
                m_buffer[i] = static_cast<uint8_t>(255 - m_buffer[i]);
        }
        return;
    }
    if (m_variant == Variant::Edges)
    {
        renderEdges();
        return;
    }

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

    // Sync flash: first 100 ms of every whole MediaClock second (#196).
    const bool flash = (timestampUs % 1000000LL) < 100000LL;
    const uint32_t flashY0 = m_height - m_height / 8;
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
            // Sync band: white during the flash window, black otherwise.
            if (y >= flashY0)
            {
                r = g = b = flash ? 255 : 0;
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

    // Pace like a real device: one frame per 1/fps slot of MediaClock.
    // Repeated calls inside the same slot report "no new frame".
    const int64_t now = MediaClock::nowUs();
    const uint32_t fps = m_fps ? m_fps : 60;
    if (m_epochUs == 0)
    {
        // Align slot 0 to a whole second so flashes land on frame slots.
        m_epochUs = now - (now % 1000000LL);
    }
    const int64_t slot = ((now - m_epochUs) * static_cast<int64_t>(fps)) / 1000000LL;
    if (slot == m_lastSlot)
        return false;
    m_lastSlot = slot;
    const int64_t timestampUs = m_epochUs + (slot * 1000000LL) / static_cast<int64_t>(fps);

    renderPattern(m_frameCounter.fetch_add(1, std::memory_order_relaxed), timestampUs);
    frame.timestampUs = timestampUs;
    frame.data = m_buffer.data();
    frame.size = static_cast<size_t>(m_width) * m_height * 3;
    frame.width = m_width;
    frame.height = m_height;
    frame.format = 0; // RGB24 → FrameProcessor uploads GL_RGB directly
    return true;
}
