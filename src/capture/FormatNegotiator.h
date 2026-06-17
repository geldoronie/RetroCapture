#pragma once

#include <cstdint>
#include <string>

// #161 — FormatNegotiator: the pure capture-format decision logic that
// VideoCaptureV4L2::setFormat used to carry inline. The platform-specific
// ioctl calls (VIDIOC_ENUM_FMT / VIDIOC_S_FMT) stay in the backend; this
// helper just makes the choice and reports it, so the policy is in one
// testable place.
//
// Scoped to the V4L2 fallback chain: the DirectShow backend doesn't enumerate
// or retry candidates (it requests RGB24 and lets DirectShow's intelligent
// connect insert converters), so there is no shared retry logic to unify —
// see #161 for the rationale. Kept dependency-free (no <linux/videodev2.h>)
// so it builds on every platform; the YUYV/MJPG fourcc values it compares
// against are defined locally and equal V4L2_PIX_FMT_YUYV / V4L2_PIX_FMT_MJPEG.
namespace rc
{
namespace capture
{
class FormatNegotiator
{
public:
    // v4l2_fourcc('Y','U','Y','V') / ('M','J','P','G') computed without
    // <linux/videodev2.h> so this stays cross-platform.
    static constexpr uint32_t FOURCC_YUYV =
        uint32_t('Y') | (uint32_t('U') << 8) | (uint32_t('Y') << 16) | (uint32_t('V') << 24);
    static constexpr uint32_t FOURCC_MJPG =
        uint32_t('M') | (uint32_t('J') << 8) | (uint32_t('P') << 16) | (uint32_t('G') << 24);

    // Human-readable 4-character fourcc (for logs).
    static std::string fourccToString(uint32_t fourcc);

    // Choose the default capture format when the caller didn't request one.
    // Inputs are what the backend learned by enumerating the device:
    //   yuyvSupported / mjpegSupported — VIDIOC_ENUM_FMT results
    //   deviceCurrentFormat            — the device's current pixelformat (may be 0)
    // Returns the chosen fourcc. Sets ok=false (and logs an error) when no usable
    // format exists. Emits the same INFO/WARN messages as the original inline code.
    static uint32_t chooseDefaultV4L2Format(bool yuyvSupported, bool mjpegSupported,
                                            uint32_t deviceCurrentFormat, bool &ok);

    // After the driver applied the format: returns true when `actual` is an
    // acceptable result for `requested`. When they differ it logs a warning, and
    // it returns false only for the requested-YUYV / got-MJPG case (logging an
    // error), matching the original setFormat() verification.
    static bool isAcceptedFormat(uint32_t requested, uint32_t actual);
};
} // namespace capture
} // namespace rc
