#pragma once

#include "IVideoCapture.h"
#include <atomic>
#include <cstdint>
#include <vector>

/**
 * Synthetic capture source for the refactor smoke-test (#149).
 *
 * Generates a deterministic RGB24 test pattern with no hardware: vertical
 * SMPTE-style colour bars (so a regression that loses chroma or swaps
 * channels is detectable) plus a block that marches across the frame each
 * call (so a frozen/duplicated frame is detectable). Selected with
 * `--source test`. It is NOT in the platform factory and never touches a
 * real capture backend, so it can't regress the production capture path —
 * it only adds an isolated, content-known input the smoke-test can assert on.
 */
class VideoCaptureTestPattern : public IVideoCapture
{
public:
    VideoCaptureTestPattern();
    ~VideoCaptureTestPattern() override;

    bool open(const std::string &device) override;
    void close() override;
    bool isOpen() const override { return m_open; }
    bool setFormat(uint32_t width, uint32_t height, uint32_t pixelFormat = 0) override;
    bool setFramerate(uint32_t fps) override;
    bool captureFrame(Frame &frame) override;
    bool captureLatestFrame(Frame &frame) override { return captureFrame(frame); }

    bool setControl(const std::string &, int32_t) override { return false; }
    bool getControl(const std::string &, int32_t &) override { return false; }
    bool getControlMin(const std::string &, int32_t &) override { return false; }
    bool getControlMax(const std::string &, int32_t &) override { return false; }
    bool getControlDefault(const std::string &, int32_t &) override { return false; }
    std::vector<DeviceInfo> listDevices() override;
    void setDummyMode(bool enabled) override { m_dummy = enabled; }
    bool isDummyMode() const override { return m_dummy; }

    bool startCapture() override;
    void stopCapture() override;
    uint32_t getWidth() const override { return m_width; }
    uint32_t getHeight() const override { return m_height; }
    uint32_t getPixelFormat() const override { return 0; } // RGB24

private:
    void renderPattern();

    uint32_t m_width = 1280;
    uint32_t m_height = 720;
    uint32_t m_fps = 60;
    bool m_open = false;
    bool m_capturing = false;
    bool m_dummy = false;
    std::atomic<uint64_t> m_frameCounter{0};
    std::vector<uint8_t> m_buffer; // RGB24, width*height*3
};
