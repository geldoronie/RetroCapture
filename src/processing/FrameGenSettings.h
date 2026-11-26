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
 *  - Fill:     match the capture rate (#256): a game drawing below the
 *              signal rate (30 / 20 fps on a 60 Hz signal) gets its repeated
 *              captures replaced by in-between frames, round(span / capture
 *              period) slots per game frame (1..6). A game at the capture
 *              rate passes through untouched, with no added latency.
 *
 * Repeated frames (#256): with skipRepeats on, a captured frame identical to
 * the previous distinct one (the game did not draw a new picture) is not a
 * new source frame, so the source rate and the pairs follow the game's own
 * cadence instead of the capture's.
 *
 * Method — how an in-between frame is synthesised:
 *  - Blend:  plain linear blend of the two source frames (ghosts on motion).
 *  - Motion: GPU optical flow ("fast" mode): bilateral block matching on a
 *            downscaled luma pyramid, bidirectional warp of both frames to
 *            the intermediate time, confidence fallback to the blend.
 *  - RIFE:   neural interpolation ("quality" mode, #248): RIFE 4.x on ncnn +
 *            Vulkan, on a worker thread. Needs a Vulkan device and a build
 *            with ENABLE_RIFE; frames it cannot deliver in time (or at all)
 *            are synthesised with Motion.
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
        Fill = 5,
    };
    enum Method : int
    {
        Blend = 0,
        Motion = 1,
        RIFE = 2,
    };
    // #260 — the size RIFE runs at: Auto reduces above 720p (the network
    // gets a ~1280x720 copy and its frame is scaled back up), Full keeps the
    // native size, Fast always reduces.
    enum RifeScale : int
    {
        RifeAuto = 0,
        RifeFull = 1,
        RifeFast = 2,
    };

    int mode = Off;
    int method = Motion;
    // Keep regions that do not change between the two frames (HUDs, score
    // counters, letterbox) exactly still instead of warping them.
    bool protectStatic = true;
    // Ignore captures that repeat the previous picture (#256).
    bool skipRepeats = true;
    int rifeScale = RifeAuto;

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
               skipRepeats == o.skipRepeats && rifeScale == o.rifeScale &&
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
        case Fill: return "fill";
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
        if (n == "fill") return Fill;
        return -1;
    }
    static const char *methodName(int m) { return m == Blend ? "blend" : (m == RIFE ? "rife" : "motion"); }
    static int methodFromName(const std::string &n)
    {
        if (n == "blend") return Blend;
        if (n == "motion") return Motion;
        if (n == "rife") return RIFE;
        return -1;
    }
    static const char *rifeScaleName(int s) { return s == RifeFull ? "full" : (s == RifeFast ? "fast" : "auto"); }
    static int rifeScaleFromName(const std::string &n)
    {
        if (n == "auto") return RifeAuto;
        if (n == "full") return RifeFull;
        if (n == "fast") return RifeFast;
        return -1;
    }
    static int clampRifeScale(int s) { return (s >= RifeAuto && s <= RifeFast) ? s : RifeAuto; }
    static int clampMode(int m) { return (m >= Off && m <= Fill) ? m : Off; }
    static int clampMethod(int m) { return (m == Blend || m == Motion || m == RIFE) ? m : Motion; }
};
