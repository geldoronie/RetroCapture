#include "FpsOverlay.h"
#include "QuickActionsOverlay.h"
#include "../ui/UIManager.h"
#include "../ui/UIShortcutsHelp.h"

#include <imgui.h>
#include <imgui_internal.h> // BringWindowToDisplayFront

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
const ImVec4 kColLabel(0.60f, 0.60f, 0.65f, 1.0f);
const ImVec4 kColGood(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kColWarn(0.95f, 0.80f, 0.30f, 1.0f);
const ImVec4 kColBad(0.95f, 0.45f, 0.40f, 1.0f);
const ImVec4 kColIdle(0.55f, 0.55f, 0.55f, 1.0f);

// Encoder counters belong to objects that can be recreated (stream
// restart); a counter that went backwards means a fresh encoder.
uint64_t counterDelta(uint64_t now, uint64_t start)
{
    return now >= start ? now - start : now;
}

// Green when the measured rate is within 2 % of the target, yellow
// within 10 %, red below that. No target → neutral.
ImVec4 rateColor(float fps, float target)
{
    if (target <= 0.0f) return ImVec4(1, 1, 1, 1);
    const float ratio = fps / target;
    if (ratio >= 0.98f) return kColGood;
    if (ratio >= 0.90f) return kColWarn;
    return kColBad;
}
} // namespace

FpsOverlay::FpsOverlay(UIManager *uiManager,
                       QuickActionsOverlay *quickActions,
                       UIShortcutsHelp *shortcutsHelp)
    : m_uiManager(uiManager)
    , m_quickActions(quickActions)
    , m_shortcutsHelp(shortcutsHelp)
{
}

FpsOverlay::~FpsOverlay() = default;

void FpsOverlay::tick()
{
    const Clock::time_point now = Clock::now();
    const PerfCounters &c = m_counters;

    if (!m_started)
    {
        m_started        = true;
        m_lastFrame      = now;
        m_windowStart    = now;
        m_lastSeq        = c.captureSeq;
        m_lastTsUs       = c.captureTsUs;
        m_winStreamStart = c.streamEncoded;
        m_winRawStart    = c.rawEncoded;
        m_winRecordStart = c.recordEncoded;
        m_winGenRealStart = c.genReal;
        m_winGenRepeatsStart = c.genRepeats;
        m_winGenStart     = c.genGenerated;
        m_winGenDropStart = c.genDropped;
        return;
    }

    // Render: time between consecutive presented frames.
    const double ftMs = std::chrono::duration<double, std::milli>(now - m_lastFrame).count();
    m_lastFrame = now;
    if (m_winFrames == 0)
    {
        m_winMinMs = m_winMaxMs = ftMs;
    }
    else
    {
        m_winMinMs = std::min(m_winMinMs, ftMs);
        m_winMaxMs = std::max(m_winMaxMs, ftMs);
    }
    m_winSumMs += ftMs;
    ++m_winFrames;
    m_history[m_historyPos] = static_cast<float>(ftMs);
    m_historyPos = (m_historyPos + 1) % kHistory;

    // Capture: one step per new FrameProcessor sequence. The capture
    // timestamp gap against the nominal interval tells how many device
    // frames never reached the render loop (dropped) and whether a
    // "new" frame carried no new capture time (duplicate).
    if (c.captureSeq != m_lastSeq)
    {
        const uint64_t seqStep = c.captureSeq > m_lastSeq ? c.captureSeq - m_lastSeq : 1;
        if (c.captureSeq > m_lastSeq && m_lastTsUs > 0 && c.captureTsUs > 0)
        {
            const int64_t gapUs = c.captureTsUs - m_lastTsUs;
            if (gapUs <= 0)
            {
                ++m_winDuplicated;
            }
            else
            {
                m_winCapSpanUs += gapUs;
                m_winCapFrames += static_cast<uint32_t>(seqStep);
                if (c.captureNominalFps > 0.0f)
                {
                    const double expectedUs = 1e6 / c.captureNominalFps;
                    const double slots = static_cast<double>(gapUs) / expectedUs;
                    if (slots >= 1.5)
                    {
                        const int64_t missed = std::llround(slots) - static_cast<int64_t>(seqStep);
                        if (missed > 0) m_winDropped += static_cast<uint32_t>(missed);
                    }
                }
            }
        }
        m_lastSeq  = c.captureSeq;
        m_lastTsUs = c.captureTsUs;
    }

    const double windowSec = std::chrono::duration<double>(now - m_windowStart).count();
    if (windowSec >= 1.0)
    {
        publishWindow(windowSec);
        m_windowStart    = now;
        m_winFrames      = 0;
        m_winSumMs       = 0.0;
        m_winCapFrames   = 0;
        m_winCapSpanUs   = 0;
        m_winDropped     = 0;
        m_winDuplicated  = 0;
        m_winStreamStart = c.streamEncoded;
        m_winRawStart    = c.rawEncoded;
        m_winRecordStart = c.recordEncoded;
        m_winGenRealStart = c.genReal;
        m_winGenRepeatsStart = c.genRepeats;
        m_winGenStart     = c.genGenerated;
        m_winGenDropStart = c.genDropped;
    }
}

void FpsOverlay::publishWindow(double windowSec)
{
    const PerfCounters &c = m_counters;
    FpsStats s;
    s.valid      = true;
    s.renderFps  = static_cast<float>(m_winFrames / windowSec);
    s.frameMinMs = static_cast<float>(m_winMinMs);
    s.frameMaxMs = static_cast<float>(m_winMaxMs);
    s.frameAvgMs = m_winFrames ? static_cast<float>(m_winSumMs / m_winFrames) : 0.0f;

    // Measured from capture timestamps (not wall time) so a device
    // running at 59.94 reads as 59.94, not as a jittery 59/60/61.
    s.captureFps = m_winCapSpanUs > 0
                       ? static_cast<float>(m_winCapFrames * 1e6 / static_cast<double>(m_winCapSpanUs))
                       : 0.0f;
    s.captureNominalFps = c.captureNominalFps;
    m_droppedTotal    += m_winDropped;
    m_duplicatedTotal += m_winDuplicated;
    s.captureDropped         = m_winDropped;
    s.captureDuplicated      = m_winDuplicated;
    s.captureDroppedTotal    = m_droppedTotal;
    s.captureDuplicatedTotal = m_duplicatedTotal;

    s.streamActive = c.streamActive;
    s.streamFps    = static_cast<float>(counterDelta(c.streamEncoded, m_winStreamStart) / windowSec);
    s.rawActive    = c.rawActive;
    s.rawFps       = static_cast<float>(counterDelta(c.rawEncoded, m_winRawStart) / windowSec);
    s.recordActive = c.recordActive;
    s.recordFps    = static_cast<float>(counterDelta(c.recordEncoded, m_winRecordStart) / windowSec);

    s.genActive    = c.genActive;
    s.genRealFps   = static_cast<float>(counterDelta(c.genReal, m_winGenRealStart) / windowSec);
    s.genFps       = static_cast<float>(counterDelta(c.genGenerated, m_winGenStart) / windowSec);
    s.genDropped   = static_cast<uint32_t>(counterDelta(c.genDropped, m_winGenDropStart));
    s.genTargetFps = c.genTargetFps;
    s.genSourceFps = c.genSourceFps;
    s.genRepeatFps = static_cast<float>(counterDelta(c.genRepeats, m_winGenRepeatsStart) / windowSec);

    m_display = s;
    std::lock_guard<std::mutex> lock(m_statsMutex);
    m_published = s;
}

FpsStats FpsOverlay::getStats() const
{
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_published;
}

void FpsOverlay::render()
{
    if (!m_uiManager || !m_uiManager->getFpsOverlayEnabled()) return;

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const int corner = m_uiManager->getFpsOverlayCorner();
    const bool right  = (corner == 1 || corner == 3);
    const bool bottom = (corner == 2 || corner == 3);
    const float pad = 16.0f;

    float x = right ? vp->WorkPos.x + vp->WorkSize.x - pad : vp->WorkPos.x + pad;
    float y = bottom ? vp->WorkPos.y + vp->WorkSize.y - pad : vp->WorkPos.y + pad;

    // Stay clear of the other corner widgets: quick actions owns the
    // bottom-right, the shortcuts card the top-right.
    if (corner == 3 && m_quickActions && m_quickActions->isVisible())
    {
        const float h = m_quickActions->renderedHeight();
        if (h > 0.0f) y -= h + 8.0f;
    }
    if (corner == 1 && m_shortcutsHelp && m_shortcutsHelp->isVisible() &&
        m_uiManager->isVisible())
    {
        const float h = m_shortcutsHelp->renderedHeight();
        if (h > 0.0f) y += h + 8.0f;
    }

    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always,
                            ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);

    // NoInputs: purely informational, must never steal a click from
    // the video area or the windows underneath.
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration    | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav           | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoInputs;

    if (ImGui::Begin("##fpsOverlay", nullptr, flags))
    {
        // A HUD: keep it above the config windows without taking focus
        // (it is drawn before them each frame and never gets focused).
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        if (!m_display.valid)
            ImGui::TextColored(kColIdle, "FPS: measuring...");
        else if (m_uiManager->getFpsOverlayCompact())
            renderCompact();
        else
            renderFull();
    }
    ImGui::End();
}

void FpsOverlay::renderCompact()
{
    const FpsStats &s = m_display;
    const float nominal = s.captureNominalFps;

    ImGui::TextColored(kColLabel, "CAP");
    ImGui::SameLine();
    ImGui::TextColored(rateColor(s.captureFps, nominal), "%.2f", s.captureFps);
    if (s.captureDropped > 0)
    {
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::TextColored(kColBad, "(-%u)", s.captureDropped);
    }
    ImGui::SameLine();
    ImGui::TextColored(kColLabel, "REN");
    ImGui::SameLine();
    ImGui::TextColored(rateColor(s.renderFps, nominal), "%.1f", s.renderFps);
    ImGui::SameLine();
    ImGui::Text("%.1fms", s.frameAvgMs);
    if (s.genActive)
    {
        // #247 — output timeline: real + generated.
        ImGui::SameLine();
        ImGui::TextColored(kColLabel, "GEN");
        ImGui::SameLine();
        const float out = s.genRealFps + s.genFps;
        ImGui::TextColored(rateColor(out, s.genTargetFps), "%.0f+%.0f", s.genRealFps, s.genFps);
        if (s.genDropped > 0)
        {
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::TextColored(kColBad, "(-%u)", s.genDropped);
        }
    }
    if (s.streamActive)
    {
        ImGui::SameLine();
        ImGui::TextColored(kColLabel, "STR");
        ImGui::SameLine();
        ImGui::TextColored(rateColor(s.streamFps, nominal), "%.0f", s.streamFps);
    }
    if (s.rawActive)
    {
        ImGui::SameLine();
        ImGui::TextColored(kColLabel, "RAW");
        ImGui::SameLine();
        ImGui::TextColored(rateColor(s.rawFps, nominal), "%.0f", s.rawFps);
    }
    if (s.recordActive)
    {
        ImGui::SameLine();
        ImGui::TextColored(kColLabel, "REC");
        ImGui::SameLine();
        ImGui::TextColored(rateColor(s.recordFps, nominal), "%.0f", s.recordFps);
    }
}

void FpsOverlay::renderFull()
{
    const FpsStats &s = m_display;
    const float nominal = s.captureNominalFps;
    const float col = 72.0f;

    // Capture
    ImGui::TextColored(kColLabel, "Capture");
    ImGui::SameLine(col);
    ImGui::TextColored(rateColor(s.captureFps, nominal), "%6.2f", s.captureFps);
    ImGui::SameLine();
    if (nominal > 0.0f)
        ImGui::TextColored(kColLabel, "/ %.2f fps", nominal);
    else
        ImGui::TextColored(kColLabel, "fps");
    if (s.captureDropped > 0 || s.captureDuplicated > 0 ||
        s.captureDroppedTotal > 0 || s.captureDuplicatedTotal > 0)
    {
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::SameLine(col);
        ImGui::TextColored(s.captureDropped ? kColBad : kColLabel,
                           "drop %u (%llu)", s.captureDropped,
                           static_cast<unsigned long long>(s.captureDroppedTotal));
        ImGui::SameLine();
        ImGui::TextColored(s.captureDuplicated ? kColWarn : kColLabel,
                           "dup %u (%llu)", s.captureDuplicated,
                           static_cast<unsigned long long>(s.captureDuplicatedTotal));
    }

    // Render
    ImGui::TextColored(kColLabel, "Render");
    ImGui::SameLine(col);
    ImGui::TextColored(rateColor(s.renderFps, nominal), "%6.2f", s.renderFps);
    ImGui::SameLine();
    ImGui::TextColored(kColLabel, "fps");
    ImGui::TextColored(kColLabel, "Frame");
    ImGui::SameLine(col);
    ImGui::Text("%.2f / %.2f / %.2f ms", s.frameMinMs, s.frameAvgMs, s.frameMaxMs);
    ImGui::SameLine();
    ImGui::TextColored(kColLabel, "min/avg/max");

    // Frame generation (#247): the output timeline, real vs generated.
    if (s.genActive)
    {
        const float out = s.genRealFps + s.genFps;
        ImGui::TextColored(kColLabel, "Output");
        ImGui::SameLine(col);
        ImGui::TextColored(rateColor(out, s.genTargetFps), "%6.2f", out);
        ImGui::SameLine();
        if (s.genTargetFps > 0.0f)
            ImGui::TextColored(kColLabel, "/ %.2f fps", s.genTargetFps);
        else
            ImGui::TextColored(kColLabel, "fps (per refresh)");
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::SameLine(col);
        ImGui::TextColored(kColLabel, "real %.1f + gen %.1f", s.genRealFps, s.genFps);
        if (s.genDropped > 0)
        {
            ImGui::SameLine();
            ImGui::TextColored(kColBad, "drop %u", s.genDropped);
        }
        // #256 — the game's own rate when it draws below the capture rate.
        if (s.genRepeatFps > 0.5f)
        {
            ImGui::TextColored(kColLabel, "Game");
            ImGui::SameLine(col);
            ImGui::Text("%6.2f", s.genSourceFps);
            ImGui::SameLine();
            ImGui::TextColored(kColLabel, "fps (%.0f repeats/s skipped)", s.genRepeatFps);
        }
    }

    // Encoders (only while they run)
    auto encoderRow = [&](const char *label, float fps) {
        ImGui::TextColored(kColLabel, "%s", label);
        ImGui::SameLine(col);
        ImGui::TextColored(rateColor(fps, nominal), "%6.2f", fps);
        ImGui::SameLine();
        ImGui::TextColored(kColLabel, "fps");
    };
    if (s.streamActive) encoderRow("Stream", s.streamFps);
    if (s.rawActive)    encoderRow("Raw", s.rawFps);
    if (s.recordActive) encoderRow("Record", s.recordFps);

    if (m_uiManager->getFpsOverlayGraph())
    {
        // Scale: at least two nominal frame intervals so a steady
        // stream sits mid-graph and a spike is obvious.
        float scaleMax = nominal > 0.0f ? 2000.0f / nominal : 33.3f;
        scaleMax = std::max(scaleMax, s.frameMaxMs * 1.1f);
        ImGui::PlotLines("##frametime", m_history, kHistory, m_historyPos,
                         nullptr, 0.0f, scaleMax, ImVec2(240.0f, 40.0f));
    }
}
