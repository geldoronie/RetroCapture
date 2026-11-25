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
 *
 * A/V sync test (#196): frames are paced at the configured fps off
 * MediaClock (a new frame only when its slot is due, like a real device)
 * and carry their ideal capture timestamp. The bottom band of the frame
 * flashes white for the first 100 ms of every whole MediaClock second —
 * the same instants AudioCaptureTestTone beeps — so the output's A/V
 * offset can be measured from the flash/beep onsets.
 *
 * RETROCAPTURE_TEST_PATTERN selects an alternative layout for image-path
 * tests (default: the bars above):
 *  - "levels" (#228): top half = 8 grey steps (0,16,32,64,128,192,235,255),
 *    bottom half = the colour bars. Measures range/saturation/gamma.
 *  - "interlaced" (#229): synthetic woven interlaced frames, top field
 *    first. A white bar moves 16 px per FIELD on black: even rows hold the
 *    bar at field time 2f, odd rows at 2f+1, so motion combs exactly like a
 *    480i/576i capture. Measures deinterlacing.
 *  - "motion" (#247): game-like moving content for frame generation — a
 *    textured background scrolling 240 px/s with parallax, three sprites
 *    on curved paths, a static HUD bar with a counter that changes twice a
 *    second, plus the sync flash band. Positions are a pure function of
 *    the frame timestamp (see renderMotion), so any instant can be
 *    rendered exactly: frame generation is measured against it.
 *  - "edges" (#251): a static, hard-edged pattern defined in normalised
 *    coordinates (concentric rings, a rotated checkerboard, a colour
 *    starburst), so the same pattern rendered at a higher --width/--height
 *    is the ground truth of an upscale. Measures upscalers.
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

    /**
     * #247 — the "motion" picture at time tSec (RGB24, w*h*3 bytes). The
     * sync band at the bottom flashes when `flash` is set. Static so the
     * frame-generation bench renders the exact same content offline.
     */
    static void renderMotion(uint8_t *rgb, uint32_t w, uint32_t h, double tSec, bool flash);

private:
    void renderPattern(uint64_t frameIndex, int64_t timestampUs);
    void renderLevels();
    void renderEdges();
    void renderInterlaced(uint64_t frameIndex);

    enum class Variant { Bars, Levels, Interlaced, Motion, Edges };
    Variant m_variant = Variant::Bars;

    uint32_t m_width = 1280;
    uint32_t m_height = 720;
    uint32_t m_fps = 60;
    bool m_open = false;
    bool m_capturing = false;
    bool m_dummy = false;
    std::atomic<uint64_t> m_frameCounter{0};
    int64_t m_epochUs = 0;          // MediaClock time of frame slot 0
    int64_t m_lastSlot = -1;        // last frame slot delivered
    std::vector<uint8_t> m_buffer; // RGB24, width*height*3
};
