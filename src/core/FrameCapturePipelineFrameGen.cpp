// #247 — frame generation for FrameCapturePipeline, kept in its own
// translation unit so the render body only carries the call site (right after
// the source crop pre-pass, see renderAndDistributeFrame()).
#include "FrameCapturePipeline.h"
#include "Application.h"
#include "../processing/FrameGenSettings.h"
#include "../renderer/PBOManager.h"
#include "../processing/FrameProcessor.h"
#include "../recording/RecordingManager.h"
#include "../recording/RecordingSettings.h"
#include "../streaming/StreamManager.h"
#include "../ui/UIManager.h"
#include "../utils/Logger.h"
#include "../utils/MediaClock.h"
#ifdef USE_SDL2
#include "../output/WindowManagerSDL.h"
#else
#include "../output/WindowManager.h"
#endif
#if defined(__linux__)
#include "../output/VirtualCameraOutput.h"
#elif defined(_WIN32)
#include "../output/VirtualCameraOutputWin.h"
#elif defined(__APPLE__)
#include "../output/VirtualCameraOutputMac.h"
#endif

#include <algorithm>
#include <cmath>
#include <string>

// GL objects are released by releaseGL() during Application::shutdown(),
// while the context is still current.
FrameCapturePipeline::~FrameCapturePipeline() = default;

void FrameCapturePipeline::releaseGL()
{
    if (m_frameGen)
    {
        m_frameGen->destroy();
        m_frameGen.reset();
    }
    if (m_genPbo)
    {
        m_genPbo->cleanup();
        m_genPbo.reset();
    }
}

namespace
{
// #248 — the RIFE backend's state for the UI / web API.
void fillRifeStatus(UIManager::FrameGenStatus &st, const FrameInterpolator &fi, bool selected)
{
    const RifeBackend &r = fi.rife();
    st.rifeCompiled = RifeBackend::compiledIn();
    const RifeBackend::State s = r.state();
    st.rifeState = static_cast<int>(s);
    if (!st.rifeCompiled)
        st.rifeReason = "this build has no RIFE support (built without ncnn/Vulkan)";
    else if (s == RifeBackend::State::Unavailable)
        st.rifeReason = r.reason();
    st.rifeDevice = r.device();
    if (selected && s == RifeBackend::State::Ready)
    {
        const RifeBackend::Stats rs = r.stats();
        st.rifeInferMs = static_cast<float>(rs.inferMs);
        st.rifeLatencyMs = static_cast<float>(rs.latencyMs);
        st.rifeDelivered = rs.delivered;
        st.rifeLate = rs.late;
    }
}
} // namespace

FrameCapturePipeline::FrameGenPlan FrameCapturePipeline::planFrameGeneration(GLuint srcTex, uint32_t w,
                                                                             uint32_t h, const FrameMeta &live)
{
    FrameGenPlan plan;
    UIManager *ui = m_app.m_ui.get();
    if (!ui)
        return plan;

    FrameGenSettings fg = ui->getFrameGenSettings();
    UIManager::FrameGenStatus st;
    const bool remote = ui->getSourceType() == UIManager::SourceType::Remote;
    if (remote)
    {
        // A remote source's frame generation is the "motion" entry of the
        // remote interpolation dropdown: per refresh, window only. The
        // client's own stream / recording / virtual camera keep real frames.
        if (m_app.m_remoteInterpolation == "motion")
        {
            fg.mode = FrameGenSettings::Display;
            fg.method = FrameGenSettings::Motion;
            fg.window = true;
            fg.stream = fg.record = fg.virtcam = false;
            st.remoteDriven = true;
        }
        else
        {
            fg.mode = FrameGenSettings::Off;
        }
    }
    const bool display = fg.mode == FrameGenSettings::Display;
    if (display)
        fg.stream = fg.record = fg.virtcam = false; // per-refresh is window only

    const bool streamActive = m_app.m_streamManager && m_app.m_streamManager->isActive();
    const bool recording = m_app.m_recordingManager && m_app.m_recordingManager->isRecording();
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
    const bool virtcamRunning = m_app.m_virtcam && m_app.m_virtcam->isRunning();
#else
    const bool virtcamRunning = false;
#endif
    const bool consumers = fg.mode != FrameGenSettings::Off &&
                           (fg.window || (fg.stream && streamActive) || (fg.record && recording) ||
                            (fg.virtcam && virtcamRunning));

    auto restoreVsync = [&]() {
        if (m_frameGenVsync && m_app.m_window && !remote)
            m_app.m_window->setVsync(false); // the local-source default
        m_frameGenVsync = false;
    };

    if (!m_frameGen)
        m_frameGen = std::make_unique<FrameGenStage>();
    FrameInterpolator &fi = m_frameGen->interpolator();
    const bool supported = consumers ? fi.isSupported() : true;
    st.supported = supported;
    st.motionSupported = supported && (consumers ? fi.motionSupported() : true);
    st.effectiveMode = fg.mode;
    if (!consumers || !supported || srcTex == 0 || w == 0 || h == 0)
    {
        m_frameGen->deactivate();
        restoreVsync();
        if (m_frameGenLoggedMode != 0)
        {
            if (m_frameGenLoggedMode > 0)
                LOG_INFO("Frame generation: off");
            m_frameGenLoggedMode = 0;
        }
        fillRifeStatus(st, fi, false);
        ui->setFrameGenStatus(st);
        return plan;
    }

    // Present the generated / copied frames with the filter the source
    // texture has: the FrameProcessor's own, NEAREST for the crop target.
    const bool srcIsProcessor = m_app.m_frameProcessor && srcTex == m_app.m_frameProcessor->getTexture();
    fi.setFilterLinear(srcIsProcessor && m_app.m_frameProcessor->getTextureFilterLinear());

    FrameInterpolator::Options opt;
    opt.method = fg.method == FrameGenSettings::Blend  ? FrameInterpolator::Method::Blend
                 : fg.method == FrameGenSettings::RIFE ? FrameInterpolator::Method::RIFE
                                                       : FrameInterpolator::Method::Motion;
    opt.protectStatic = fg.protectStatic;
    const bool fill = fg.mode == FrameGenSettings::Fill;
    const int factorIn = display ? 0 : fill ? FrameGenStage::kFactorFill : fg.factor();
    const FrameGenStage::Frame frame = m_frameGen->update(srcTex, w, h, live.seq, live.tsUs, MediaClock::nowUs(),
                                                          factorIn, fg.skipRepeats, opt);
    const int factor = display ? 0 : m_frameGen->effectiveFactor();

    // Source rate = distinct frames (the game's own rate with skipRepeats).
    // Source rate: the learnt game cadence when repeats are skipped (the
    // clock's running average also counts pauses and held-through repeats,
    // so it reads low while the picture is still), else the clock's period.
    const int64_t cadence = fg.skipRepeats ? m_frameGen->cadenceUs() : 0;
    const int64_t period = cadence > 0 ? cadence : m_frameGen->clock().periodUs();
    const int64_t capPeriod = m_frameGen->capturePeriodUs();
    const double srcFps = period > 0 ? 1e6 / static_cast<double>(period) : 0.0;
    const double capFps = capPeriod > 0 ? 1e6 / static_cast<double>(capPeriod) : 0.0;
    st.active = true;
    st.sourceFps = static_cast<float>(srcFps);
    st.captureFps = static_cast<float>(capFps);
    st.repeatsSkipped = m_frameGen->repeatsSkipped();
    st.sceneCuts = m_frameGen->sceneCuts();
    st.sourceWidth = w;
    st.sourceHeight = h;
    if (display)
        st.outputFps = 0.0f;
    else if (fill)
        st.outputFps = static_cast<float>(capFps > 0.0 ? std::max(capFps, srcFps) : srcFps);
    else
        st.outputFps = static_cast<float>(srcFps * factor);
    st.addedLatencyMs = static_cast<float>(
        display ? period / 1000.0 : (period * (factor - 1)) / (1000.0 * (factor > 0 ? factor : 1)));
    st.addedLatencyMs += static_cast<float>(m_frameGen->clock().extraDelayUs() / 1000.0);
    fillRifeStatus(st, fi, fg.method == FrameGenSettings::RIFE);
    ui->setFrameGenStatus(st);

    const int logKey = 1 + fg.mode * 16 + fg.method * 4 + (fg.skipRepeats ? 2 : 0) + (st.remoteDriven ? 1 : 0);
    if (logKey != m_frameGenLoggedMode)
    {
        m_frameGenLoggedMode = logKey;
        std::string outs;
        if (fg.window) outs += " window";
        if (fg.stream) outs += " stream";
        if (fg.record) outs += " recording";
        if (fg.virtcam) outs += " virtual-camera";
        LOG_INFO(std::string("Frame generation: ") + FrameGenSettings::modeName(fg.mode) + ", " +
                 (opt.method == FrameInterpolator::Method::RIFE ? "rife"
                  : opt.method == FrameInterpolator::Method::Motion && fi.motionSupported() ? "motion"
                                                                                           : "blend") +
                 (st.remoteDriven ? " (remote interpolation)" : "") +
                 (fg.skipRepeats ? ", skipping repeated frames" : "") + ", outputs:" + (outs.empty() ? " none" : outs));
    }

    // Display mode follows the window refresh: vsync on while focused
    // (a backgrounded window may never get a refresh, see setVsync()).
    if (display && fg.window && !remote && m_app.m_window)
    {
        const bool want = m_app.m_window->isFocused();
        if (want != m_frameGenVsync)
        {
            m_app.m_window->setVsync(want);
            m_frameGenVsync = want;
        }
    }
    else
    {
        restoreVsync();
    }

    if (!frame.valid)
        return plan; // nothing on the timeline yet: live frame this time

    // An encoder only takes the timeline phases its frame rate can carry:
    // with N phases per source frame and an encoder at M x the source rate
    // (M < N), it keeps the slots j with j * M divisible by N (e.g. 4x
    // timeline, 60 fps stream, 30 fps source -> slots 2 and 4). Real frames
    // always go out. Display mode is per refresh: real frames only.
    auto accept = [&](double outFps) {
        if (!frame.generated)
            return true;
        // Fill (#256): the timeline runs at the capture rate; an encoder at
        // (or above) that rate takes every slot, whatever the game's rate.
        if (fill && capFps > 0.0 && outFps >= capFps * 0.95)
            return true;
        if (frame.slots <= 0 || srcFps <= 0.0)
            return false;
        // Rounding tolerance: the measured source rate wanders (a 30 fps game
        // reads 30-32), and 60 / 31.5 must still carry 2 phases.
        int m = static_cast<int>(std::floor(outFps / srcFps + 0.15));
        if (m < 1) m = 1;
        if (m > frame.slots) m = frame.slots;
        return (frame.slot * m) % frame.slots == 0;
    };
    double streamFps = ui->getStreamingFps();
    if (streamFps <= 0.0) streamFps = ui->getCaptureFps();
    double recordFps = 0.0;
    if (recording)
        recordFps = m_app.m_recordingManager->getRecordingSettings().fps;
    if (recordFps <= 0.0) recordFps = ui->getCaptureFps();

    plan.active = true;
    plan.frame = frame;
    plan.genWindow = fg.window;
    plan.genStream = fg.stream;
    plan.genRecord = fg.record;
    plan.streamPhaseOk = accept(streamFps);
    plan.recordPhaseOk = accept(recordFps);
    plan.genVirtcam = fg.virtcam;
    plan.liveStream = !fg.stream;
    plan.liveRecord = !fg.record;
    plan.liveVirtcam = !fg.virtcam;
    plan.liveOutputsActive = (!fg.stream && streamActive) || (!fg.record && recording) ||
                             (!fg.virtcam && virtcamRunning) ||
                             (streamActive && m_app.m_streamManager->hasRawClients());
    return plan;
}
