#include "ScreenBackend.h"

// Fallback screen backend: a moving test pattern. Compiled only when no
// real platform grabber is enabled for this build (Linux without
// PipeWire, or a platform whose backend isn't built). The real backends
// — VideoCaptureScreen_linux.cpp (PipeWire), _win.cpp (DXGI), _mac.mm
// (ScreenCaptureKit) — take over when their macro is defined.
#if !defined(RETROCAPTURE_SCREEN_PIPEWIRE) && \
    !defined(RETROCAPTURE_SCREEN_DXGI) && \
    !defined(RETROCAPTURE_SCREEN_SCK)

#include "../utils/Logger.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace
{
class TestPatternBackend : public ScreenBackend
{
public:
    explicit TestPatternBackend(IScreenFrameSink &sink) : m_sink(sink) {}
    ~TestPatternBackend() override { stop(); }

    bool start(const std::string &, bool) override
    {
        if (m_running.exchange(true)) return true;
        m_thread = std::thread([this]() {
            const uint32_t w = 1280, h = 720;
            std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4);
            uint32_t tick = 0;
            while (m_running.load())
            {
                uint8_t *p = buf.data();
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        const size_t i = (static_cast<size_t>(y) * w + x) * 4;
                        p[i + 0] = static_cast<uint8_t>((x + tick) & 0xFF); // B
                        p[i + 1] = static_cast<uint8_t>((y + tick) & 0xFF); // G
                        p[i + 2] = static_cast<uint8_t>((x + y) & 0xFF);    // R
                        p[i + 3] = 255;
                    }
                m_sink.onScreenFrame(buf.data(), w, h, w * 4, ScreenPixelFormat::BGRA);
                ++tick;
                std::this_thread::sleep_for(std::chrono::milliseconds(33)); // ~30 fps
            }
        });
        return true;
    }

    void stop() override
    {
        if (!m_running.exchange(false)) return;
        if (m_thread.joinable()) m_thread.join();
    }

    std::vector<DeviceInfo> listTargets() override
    {
        DeviceInfo d;
        d.id        = "monitor:0";
        d.name      = "Primary display (test pattern)";
        d.driver    = "screen";
        d.available = true;
        return {d};
    }

private:
    IScreenFrameSink  &m_sink;
    std::atomic<bool>  m_running{false};
    std::thread        m_thread;
};
} // namespace

std::unique_ptr<ScreenBackend> createScreenBackend(IScreenFrameSink &sink)
{
    LOG_INFO("ScreenBackend: test-pattern stub (no platform grabber in this build)");
    return std::make_unique<TestPatternBackend>(sink);
}

#endif // no real screen backend for this build
