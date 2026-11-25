#pragma once

#include <string>

/**
 * Frame generation settings (#247). Shared by the UI, the web API, the
 * config file and the render pipeline. Defaults = off, and with the mode
 * off the pipeline is exactly the pre-#247 one.
 *
 * Mode — the output clock:
 *  - Off:      no generated frames.
 *  - X2/X3/X4: N output frames per source frame, at even phases
 *              (1/N, 2/N, ..., 1) between two source frames. Adds
 *              (N-1)/N of a source frame of latency.
 *  - Display:  one frame per window refresh (vsync) at the exact phase
 *              of that refresh, for non-integer ratios such as 60 -> 144.
 *              Window only: stream / recording / virtual camera keep the
 *              real frames in this mode. Adds one source frame of latency.
 *
 * Method — how an in-between frame is synthesised:
 *  - Blend:  plain linear blend of the two source frames (ghosts on motion).
 *  - Motion: GPU optical flow ("fast" mode): bilateral block matching on a
 *            downscaled luma pyramid, bidirectional warp of both frames to
 *            the intermediate time, confidence fallback to the blend.
 */
struct FrameGenSettings
{
    enum Mode : int
    {
        Off = 0,
        X2 = 1,
        X3 = 2,
        X4 = 3,
        Display = 4,
    };
    enum Method : int
    {
        Blend = 0,
        Motion = 1,
    };

    int mode = Off;
    int method = Motion;
    // Keep regions that do not change between the two frames (HUDs, score
    // counters, letterbox) exactly still instead of warping them.
    bool protectStatic = true;

    // Per-output switches: which outputs receive the generated frames.
    // An output switched off keeps the real frames with no added latency.
    bool window = true;
    bool stream = true;
    bool record = true;
    bool virtcam = true;

    /** Output frames per source frame for the X modes (0 for Off/Display). */
    int factor() const
    {
        switch (mode)
        {
        case X2: return 2;
        case X3: return 3;
        case X4: return 4;
        default: return 0;
        }
    }

    bool operator==(const FrameGenSettings &o) const
    {
        return mode == o.mode && method == o.method && protectStatic == o.protectStatic &&
               window == o.window && stream == o.stream && record == o.record &&
               virtcam == o.virtcam;
    }
    bool operator!=(const FrameGenSettings &o) const { return !(*this == o); }

    static const char *modeName(int m)
    {
        switch (m)
        {
        case X2: return "2x";
        case X3: return "3x";
        case X4: return "4x";
        case Display: return "display";
        default: return "off";
        }
    }
    /** -1 when the name is unknown. */
    static int modeFromName(const std::string &n)
    {
        if (n == "off") return Off;
        if (n == "2x") return X2;
        if (n == "3x") return X3;
        if (n == "4x") return X4;
        if (n == "display") return Display;
        return -1;
    }
    static const char *methodName(int m) { return m == Blend ? "blend" : "motion"; }
    static int methodFromName(const std::string &n)
    {
        if (n == "blend") return Blend;
        if (n == "motion") return Motion;
        return -1;
    }
    static int clampMode(int m) { return (m >= Off && m <= Display) ? m : Off; }
    static int clampMethod(int m) { return (m == Blend || m == Motion) ? m : Motion; }
};
