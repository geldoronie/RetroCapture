#pragma once

#include <chrono>
#include <cstdint>

/**
 * The single media clock shared by capture, encoding and playback.
 *
 * Every capture timestamp (video frames, audio chunks) and every
 * comparison against "now" in the A/V pipeline must use this clock, so
 * audio and video live on one timeline (#201). It is std::chrono's
 * steady_clock in microseconds, which maps to:
 *   - Linux:   CLOCK_MONOTONIC (same clock as V4L2 MONOTONIC buffer
 *              timestamps and PulseAudio's pa_rtclock)
 *   - Windows: QueryPerformanceCounter (same clock as WASAPI QPC positions)
 *   - macOS:   mach_absolute_time / CLOCK_UPTIME_RAW (AudioTimeStamp host time)
 */
namespace MediaClock
{
inline int64_t nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace MediaClock
