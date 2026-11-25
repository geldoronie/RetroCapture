#pragma once

#include "FrameGenClock.h"
#include "FrameInterpolator.h"
#include <cstdint>

/**
 * Frame generation stage (#247): the output clock (FrameGenClock) driving
 * the GPU interpolator (FrameInterpolator). It sits on the pre-shader source
 * (after the crop / colour pre-passes, before the shader chain), so the
 * shader chain and every output run once per OUTPUT frame.
 *
 * update() runs once per render-loop iteration while generation is active:
 * a new real frame (new FrameProcessor sequence) goes into the history and
 * the clock; then, if an output frame is due, it is synthesised (or B is
 * handed out as is for the real slot). The returned frame stays current
 * until the next one is due, with the same sequence number, so the outputs'
 * per-sequence dedupe pushes each output frame exactly once.
 */
class FrameGenStage
{
public:
    struct Frame
    {
        bool valid = false;
        GLuint tex = 0;
        uint32_t w = 0, h = 0;
        uint64_t seq = 0;   // output sequence (FrameGenClock range)
        int64_t tsUs = 0;   // interpolated capture timestamp
        bool generated = false;
        float alpha = 1.0f;
        int slot = 0, slots = 0; // fixed-multiple phase (0/0 in display mode)
        bool fresh = false;      // emitted by THIS update() call
    };

    /** factor value for the capture-rate fill mode (#256). */
    static constexpr int kFactorFill = -1;

    /**
     * factor: 2..4 = fixed multiple, 0 = per refresh (display), kFactorFill
     * = match the capture rate. skipRepeats: captures that repeat the latest
     * distinct picture are not source frames (#256).
     * Returns the current output frame (invalid until the first one).
     */
    Frame update(GLuint srcTex, uint32_t w, uint32_t h, uint64_t srcSeq, int64_t srcTsUs, int64_t nowUs,
                 int factor, bool skipRepeats, const FrameInterpolator::Options &opt);

    /** Slots per source frame the last update() scheduled (fill: per pair). */
    int effectiveFactor() const { return m_effFactor; }
    /** Capture interval (every captured frame, repeats included); 0 = unknown. */
    int64_t capturePeriodUs() const { return m_capPeriodUs; }
    /** Captures skipped as repeats so far (monotonic). */
    uint64_t repeatsSkipped() const { return m_repeats; }

    /** Generation switched off: forget the history (counters persist). */
    void deactivate();
    bool isActive() const { return m_active; }

    FrameInterpolator &interpolator() { return m_interp; }
    const FrameGenClock &clock() const { return m_clock; }

    void destroy() { m_interp.destroy(); }

private:
    // Timing stats, logged every few seconds (#247 acceptance numbers):
    // lag = wall time of presentation minus the frame's capture timestamp,
    // for output frames and, as the baseline, for real frames on arrival;
    // interval = wall time between consecutive output frames.
    struct TimingStats
    {
        int n = 0;
        double outLagSum = 0.0, liveLagSum = 0.0;
        int liveN = 0;
        double ivSum = 0.0, ivSq = 0.0, ivMin = 1e30, ivMax = 0.0;
        int ivN = 0;
        int64_t lastEmitWallUs = 0;
        int64_t windowStartUs = 0;
    };
    void noteEmit(int64_t nowUs, int64_t tsUs);
    TimingStats m_stats;

    FrameInterpolator m_interp;
    FrameGenClock m_clock;
    Frame m_current;
    uint64_t m_lastSrcSeq = 0;
    bool m_active = false;

    // #256 — capture cadence and repeat detection.
    bool acceptAsSourceFrame(GLuint srcTex, uint32_t w, uint32_t h, int64_t srcTsUs, bool skipRepeats);
    int64_t m_capPeriodUs = 0;
    int64_t m_lastCapTsUs = 0;
    int64_t m_lastDistinctTsUs = 0;
    float m_noiseFloor = 0.0f;
    bool m_noiseValid = false;
    uint64_t m_repeats = 0;
    int m_effFactor = 0;
};
