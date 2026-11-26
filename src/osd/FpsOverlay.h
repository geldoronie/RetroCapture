#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

class UIManager;
class QuickActionsOverlay;
class UIShortcutsHelp;

/**
 * Raw pipeline counters fed to the FPS overlay once per main-loop
 * iteration (#226). Everything here is a monotonic counter or a flag;
 * the overlay turns them into per-second rates itself, so producers
 * (encoder threads) only ever bump an atomic.
 */
struct PerfCounters
{
    // Capture: FrameProcessor sequence + capture time of the current
    // frame (#198), and the rate the device is configured for.
    uint64_t captureSeq        = 0;
    int64_t  captureTsUs       = 0;
    float    captureNominalFps = 0.0f;

    // Encoders: total video frames accepted by each encoder since it
    // was (re)started. *Active = the encoder is actually being fed.
    bool     streamActive    = false;
    uint64_t streamEncoded   = 0;
    bool     rawActive       = false;
    uint64_t rawEncoded      = 0;
    bool     recordActive    = false;
    uint64_t recordEncoded   = 0;

    // Frame generation (#247): frames put on the output timeline, split
    // into real (captured) and generated ones, plus slots that were due
    // but skipped because the loop ran late. Monotonic.
    bool     genActive       = false;
    uint64_t genReal         = 0;
    uint64_t genGenerated    = 0;
    uint64_t genDropped      = 0;
    float    genTargetFps    = 0.0f; // output timeline rate (0 = per refresh)
    float    genSourceFps    = 0.0f; // distinct source frames (the game's rate, #256)
    uint64_t genRepeats      = 0;    // captures skipped as repeats (#256)
};

/**
 * One second worth of derived metrics. Published once per second;
 * readable from any thread (web API) through FpsOverlay::getStats().
 */
struct FpsStats
{
    bool  valid = false;

    float renderFps   = 0.0f;
    float frameMinMs  = 0.0f;
    float frameAvgMs  = 0.0f;
    float frameMaxMs  = 0.0f;

    float    captureFps        = 0.0f;
    float    captureNominalFps = 0.0f;
    // Frames the render loop never saw (capture timestamp gaps) and
    // frames that arrived with no capture-time progress. Per window
    // and since start.
    uint32_t captureDropped         = 0;
    uint32_t captureDuplicated      = 0;
    uint64_t captureDroppedTotal    = 0;
    uint64_t captureDuplicatedTotal = 0;

    bool  streamActive    = false;
    float streamFps       = 0.0f;
    bool  rawActive       = false;
    float rawFps          = 0.0f;
    bool  recordActive    = false;
    float recordFps       = 0.0f;

    // Frame generation (#247)
    bool     genActive    = false;
    float    genRealFps   = 0.0f;
    float    genFps       = 0.0f;  // generated frames per second
    uint32_t genDropped   = 0;     // in this window
    float    genTargetFps = 0.0f;
    float    genSourceFps = 0.0f;
    float    genRepeatFps = 0.0f;  // repeats skipped per second
};

/**
 * FPS counter OSD (#226).
 *
 * Pinned to a user-selected corner of the local window; drawn with
 * ImGui after the stream / recording readback, so it never reaches
 * any encoded output. Shows where a bottleneck is rather than just
 * the window rate:
 *   - Capture: frames delivered by the device vs. the nominal rate,
 *     plus dropped / duplicate estimates from capture timestamps.
 *   - Render:  main-loop FPS and frame time min / avg / max over 1 s,
 *     with an optional frame-time graph.
 *   - Encoders: /stream, /raw and recording FPS while they run.
 *
 * tick() runs every frame (a handful of arithmetic ops, no
 * allocations) so the web API can read the stats even when the
 * overlay is hidden; render() only draws when enabled.
 */
class FpsOverlay
{
public:
    FpsOverlay(UIManager *uiManager,
               QuickActionsOverlay *quickActions,
               UIShortcutsHelp *shortcutsHelp);
    ~FpsOverlay();

    void setCounters(const PerfCounters &counters) { m_counters = counters; }

    // Advance the measurement window. Call once per presented frame.
    void tick();
    void render();

    // Thread-safe copy of the last published window.
    FpsStats getStats() const;

private:
    using Clock = std::chrono::steady_clock;

    void publishWindow(double windowSec);
    void renderFull();
    void renderCompact();

    UIManager           *m_uiManager     = nullptr;
    QuickActionsOverlay *m_quickActions  = nullptr;
    UIShortcutsHelp     *m_shortcutsHelp = nullptr;

    PerfCounters m_counters;

    // Frame-time history for the graph (ring buffer, ms).
    static constexpr int kHistory = 120;
    float m_history[kHistory] = {};
    int   m_historyPos = 0;

    // Current 1 s window.
    bool              m_started = false;
    Clock::time_point m_lastFrame;
    Clock::time_point m_windowStart;
    uint32_t m_winFrames  = 0;
    double   m_winMinMs   = 0.0;
    double   m_winMaxMs   = 0.0;
    double   m_winSumMs   = 0.0;

    uint64_t m_lastSeq        = 0;
    int64_t  m_lastTsUs       = 0;
    uint32_t m_winCapFrames   = 0;
    int64_t  m_winCapSpanUs   = 0;
    uint32_t m_winDropped     = 0;
    uint32_t m_winDuplicated  = 0;
    uint64_t m_droppedTotal   = 0;
    uint64_t m_duplicatedTotal = 0;

    uint64_t m_winStreamStart = 0;
    uint64_t m_winRawStart    = 0;
    uint64_t m_winRecordStart = 0;
    uint64_t m_winGenRealStart = 0;
    uint64_t m_winGenRepeatsStart = 0;
    uint64_t m_winGenStart     = 0;
    uint64_t m_winGenDropStart = 0;

    // Main-thread copy used for drawing, and the locked copy for the API.
    FpsStats           m_display;
    mutable std::mutex m_statsMutex;
    FpsStats           m_published;
};
