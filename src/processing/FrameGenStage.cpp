#include "FrameGenStage.h"
#include "../utils/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
// A repeat is held at most this many capture periods (= the fill slot cap,
// so down to a 10 fps game on 60 Hz): past it (a static scene, a menu) the
// capture goes through anyway, so the outputs never stall.
constexpr int64_t kMaxHoldPeriods = 6;
constexpr int kMaxFillSlots = 6;
} // namespace

bool FrameGenStage::acceptAsSourceFrame(GLuint srcTex, uint32_t w, uint32_t h, int64_t srcTsUs,
                                        bool skipRepeats)
{
    // Capture cadence over every captured frame, repeats included.
    if (m_lastCapTsUs > 0)
    {
        const int64_t d = srcTsUs - m_lastCapTsUs;
        if (d >= 2000 && d <= 250000)
        {
            if (m_capPeriodUs == 0 || d > 3 * m_capPeriodUs || 3 * d < m_capPeriodUs)
                m_capPeriodUs = d; // first interval, or the capture rate changed
            else
                m_capPeriodUs = (m_capPeriodUs * 15 + d) / 16;
        }
    }
    m_lastCapTsUs = srcTsUs;

    m_lastWasCut = false;
    if (m_interp.frameCount() < 1)
    {
        m_lastDistinctTsUs = srcTsUs;
        m_lastWasHeld = false;
        return true;
    }
    // The comparison also finds scene cuts, so it runs with repeat
    // skipping off too (one small readback per captured frame).
    const FrameInterpolator::Difference diff = m_interp.compareWithLatest(srcTex, w, h);
    if (!diff.valid)
    {
        m_lastDistinctTsUs = srcTsUs;
        m_lastWasHeld = false;
        return true;
    }
    if (diff.sceneCut())
    {
        m_lastWasCut = true;
        ++m_sceneCuts;
    }
    if (!skipRepeats)
    {
        m_lastDistinctTsUs = srcTsUs;
        m_lastWasHeld = false;
        return true;
    }
    // Usual frame-to-frame noise of this source, min-biased: repeats pull it
    // down quickly, real changes lift it only slowly.
    if (!m_noiseValid)
    {
        m_noiseFloor = diff.noise;
        m_noiseValid = true;
    }
    else if (diff.noise < m_noiseFloor)
        m_noiseFloor += (diff.noise - m_noiseFloor) / 4.0f;
    else
        m_noiseFloor += (diff.noise - m_noiseFloor) / 64.0f;

    if (m_debug)
        noteDebug(diff, srcTsUs);
    // Hold limit: ~1.5 game frames (the learnt cadence), at most
    // kMaxHoldPeriods. A picture that stays still longer (a pause, a still
    // camera) goes through as a real frame, so the pair that ends the pause
    // spans one game frame: motion resumes at its own speed instead of
    // being spread (slowed down) over the whole pause, and the latency stays
    // at about one game frame.
    int64_t holdPeriods = kMaxHoldPeriods;
    if (m_cadenceUs > 0 && m_capPeriodUs > 0)
    {
        const double c = static_cast<double>(m_cadenceUs) / static_cast<double>(m_capPeriodUs);
        holdPeriods = std::max<int64_t>(2, std::min<int64_t>(kMaxHoldPeriods, std::llround(1.5 * c)));
    }
    const bool held = m_capPeriodUs > 0 && srcTsUs - m_lastDistinctTsUs >= holdPeriods * m_capPeriodUs;
    const bool repeat = diff.repeat(m_noiseFloor);
    if (repeat && !held)
    {
        ++m_repeats;
        ++m_diffStats.repeats;
        return false;
    }
    if (repeat)
        ++m_heldThrough;
    else
        noteDistinctSpan(srcTsUs);
    m_lastWasHeld = repeat;
    m_lastDistinctTsUs = srcTsUs;
    return true;
}

bool FrameGenStage::debugEnabled()
{
    static const bool on = [] {
        const char *v = std::getenv("RETROCAPTURE_FRAMEGEN_DEBUG");
        return v && *v && std::string(v) != "0";
    }();
    return on;
}

void FrameGenStage::noteDebug(const FrameInterpolator::Difference &diff, int64_t srcTsUs)
{
    // RETROCAPTURE_FRAMEGEN_DEBUG=1: comparison percentiles in the 10 s log
    // and, with RETROCAPTURE_FRAMEGEN_DIFF_CSV=<file>, every comparison of
    // the first 3000 captures (to tune the thresholds on a new source).
    m_diffBlocks = diff.blocks;
    if (!m_csvTried)
    {
        m_csvTried = true;
        if (const char *path = std::getenv("RETROCAPTURE_FRAMEGEN_DIFF_CSV"))
            if ((m_csv = std::fopen(path, "w")))
                std::fprintf(m_csv, "ts_us,changed,noise,peak,changed_avg,peak_avg,cut_blocks\n");
    }
    if (m_csv && m_csvRows < 3000)
    {
        std::fprintf(m_csv, "%lld,%d,%.1f,%.1f,%d,%.2f,%d\n", static_cast<long long>(srcTsUs), diff.changedBlocks,
                     diff.noise * 255.0f, diff.peak * 255.0f, diff.changedAvg, diff.peakAvg * 255.0f,
                     diff.cutBlocks);
        if (++m_csvRows == 3000)
        {
            std::fclose(m_csv);
            m_csv = nullptr;
        }
        else if (m_csvRows % 60 == 0)
            std::fflush(m_csv);
    }
    if (m_diffStats.n >= 4000) m_diffStats = DiffStats(); // bounded if nothing is emitted
    m_diffStats.n++;
    m_diffStats.changed.push_back(diff.changedBlocks);
    m_diffStats.changedAvg.push_back(diff.changedAvg);
    m_diffStats.peakAvg.push_back(diff.peakAvg * 255.0f);
    m_diffStats.noise.push_back(diff.noise * 255.0f);
    m_diffStats.peak.push_back(diff.peak * 255.0f);
}

void FrameGenStage::noteDistinctSpan(int64_t srcTsUs)
{
    // Cadence = a low percentile of the recent spans between new pictures.
    // Spans that start at a held-through repeat are not the game's cadence
    // (they are its remainder) and are left out.
    const int64_t span = srcTsUs - m_lastDistinctTsUs;
    if (m_lastDistinctTsUs > 0 && !m_lastWasHeld && span >= 2000 && span <= 250000)
    {
        m_spans.push_back(span);
        if (m_spans.size() > 32) m_spans.pop_front();
        if (m_spans.size() >= 6)
        {
            std::vector<int64_t> s(m_spans.begin(), m_spans.end());
            const size_t k = s.size() * 3 / 10;
            std::nth_element(s.begin(), s.begin() + k, s.end());
            m_cadenceUs = s[k];
        }
    }
}

FrameGenStage::Frame FrameGenStage::update(GLuint srcTex, uint32_t w, uint32_t h, uint64_t srcSeq,
                                           int64_t srcTsUs, int64_t nowUs, int factor, bool skipRepeats,
                                           const FrameInterpolator::Options &opt)
{
    m_active = true;
    m_current.fresh = false;
    m_interp.setMethodHint(opt.method);

    bool pushed = false;
    if (srcTex != 0 && w > 0 && h > 0 && srcSeq != 0 && srcSeq != m_lastSrcSeq)
    {
        m_lastSrcSeq = srcSeq;
        if (w != m_interp.width() || h != m_interp.height())
        {
            // New geometry: the interpolator reallocates (and drops its
            // history), so the clock must not pair across the change.
            m_clock.reset();
            m_current = Frame();
        }
        if (acceptAsSourceFrame(srcTex, w, h, srcTsUs, skipRepeats) && m_interp.pushFrame(srcTex, w, h))
        {
            pushed = true;
            m_clock.onRealFrame(srcTsUs, nowUs);
            if (m_lastWasCut)
                m_clock.breakPair(); // scene cut: no cross-fade into the new scene
            m_stats.liveLagSum += static_cast<double>(nowUs - srcTsUs);
            ++m_stats.liveN;
        }
    }

    // Fill (#256): as many slots as capture periods the pair spans, so the
    // output runs at the capture rate (1 = the game is at the capture rate).
    if (factor == kFactorFill)
    {
        int n = 1;
        if (m_clock.hasPair() && m_capPeriodUs > 0)
        {
            n = static_cast<int>(std::llround(static_cast<double>(m_clock.spanUs()) /
                                              static_cast<double>(m_capPeriodUs)));
            n = std::max(1, std::min(kMaxFillSlots, n));
        }
        factor = n;
    }
    m_effFactor = factor;
    if (pushed && factor >= 2 && m_clock.hasPair() && opt.method == FrameInterpolator::Method::RIFE)
        m_interp.requestPhases(factor);

    // RIFE (#248) runs on a worker thread: give it the time it needs after
    // B arrives (measured, +15 %) by delaying the timeline. Capped so that B
    // itself still goes out before the next real frame is due; a slot whose
    // result is late anyway is synthesised with Motion.
    int64_t extraUs = 0;
    if (opt.method == FrameInterpolator::Method::RIFE &&
        m_interp.rife().state() == RifeBackend::State::Ready)
    {
        const double latMs = m_interp.rife().stats().latencyMs;
        extraUs = static_cast<int64_t>(latMs * 1150.0) + 1000;
        const int64_t span = m_clock.periodUs();
        if (span > 0)
        {
            const int64_t cap = factor >= 2 ? span * (factor - 1) / factor - 2000
                                : factor == 1 ? 0 : span / 2;
            if (extraUs > cap) extraUs = cap;
        }
        extraUs = (extraUs / 1000) * 1000; // whole ms: no per-frame jitter from the EMA
    }
    m_clock.setExtraDelayUs(extraUs);

    const FrameGenClock::Emit e = m_clock.poll(nowUs, factor);
    if (!e.emit)
        return m_current;

    GLuint tex = 0;
    bool generated = e.generated;
    if (generated && m_interp.frameCount() >= 2)
        tex = m_interp.render(e.alpha, opt);
    if (tex == 0)
    {
        // The real slot, or a synthesis that could not run: hand out B
        // (or A, when it is the previous B owed after C arrived).
        tex = e.previous ? m_interp.previousTexture() : m_interp.latestTexture();
        generated = false;
    }
    if (tex == 0)
        return m_current;

    m_current.valid = true;
    m_current.tex = tex;
    m_current.w = m_interp.width();
    m_current.h = m_interp.height();
    m_current.seq = e.seq;
    m_current.tsUs = e.tsUs;
    m_current.generated = generated;
    m_current.alpha = e.alpha;
    m_current.slot = e.slot;
    m_current.slots = e.slots;
    m_current.fresh = true;
    noteEmit(nowUs, e.tsUs);
    return m_current;
}

void FrameGenStage::noteEmit(int64_t nowUs, int64_t tsUs)
{
    TimingStats &s = m_stats;
    if (s.windowStartUs == 0)
        s.windowStartUs = nowUs;
    s.outLagSum += static_cast<double>(nowUs - tsUs);
    ++s.n;
    if (s.lastEmitWallUs > 0)
    {
        const double iv = static_cast<double>(nowUs - s.lastEmitWallUs) / 1000.0;
        s.ivSum += iv;
        s.ivSq += iv * iv;
        if (iv < s.ivMin) s.ivMin = iv;
        if (iv > s.ivMax) s.ivMax = iv;
        ++s.ivN;
    }
    s.lastEmitWallUs = nowUs;
    if (nowUs - s.windowStartUs < 10000000 || s.ivN < 2 || s.liveN < 1)
        return;
    const double mean = s.ivSum / s.ivN;
    const double sd = std::sqrt(std::fmax(0.0, s.ivSq / s.ivN - mean * mean));
    const double outLag = s.outLagSum / s.n / 1000.0;
    const double liveLag = s.liveLagSum / s.liveN / 1000.0;
    char buf[384];
    std::snprintf(buf, sizeof(buf),
                  "Frame generation: %d frames in %.1f s (%.2f fps), interval %.2f +- %.2f ms "
                  "[%.2f..%.2f], capture->output lag %.1f ms vs %.1f ms on arrival (added %.1f ms), "
                  "dropped so far %llu, repeats skipped %llu, held through %llu, scene cuts %llu, game cadence %.1f ms",
                  s.n, (nowUs - s.windowStartUs) / 1e6, s.n * 1e6 / static_cast<double>(nowUs - s.windowStartUs),
                  mean, sd, s.ivMin, s.ivMax, outLag, liveLag, outLag - liveLag,
                  static_cast<unsigned long long>(m_clock.dropped()),
                  static_cast<unsigned long long>(m_repeats), static_cast<unsigned long long>(m_heldThrough),
                  static_cast<unsigned long long>(m_sceneCuts),
                  m_cadenceUs / 1000.0);
    LOG_INFO(buf);
    DiffStats &d = m_diffStats;
    if (m_debug && d.n > 0)
    {
        // Percentiles 10/50/90 of the comparison against the latest
        // distinct frame: blocks changed, block noise and peak (8-bit).
        auto pct = [](auto v, double q) {
            std::sort(v.begin(), v.end());
            return static_cast<double>(v[static_cast<size_t>(q * (v.size() - 1))]);
        };
        char dbuf[320];
        std::snprintf(dbuf, sizeof(dbuf),
                      "Frame generation repeats: %d of %d captures, changed blocks p10/50/90 %.0f/%.0f/%.0f "
                      "of %d, noise %.1f/%.1f/%.1f (floor %.1f), peak %.1f/%.1f/%.1f",
                      d.repeats, d.n, pct(d.changed, 0.1), pct(d.changed, 0.5), pct(d.changed, 0.9),
                      m_diffBlocks, pct(d.noise, 0.1), pct(d.noise, 0.5), pct(d.noise, 0.9),
                      m_noiseFloor * 255.0f, pct(d.peak, 0.1), pct(d.peak, 0.5), pct(d.peak, 0.9));
        LOG_INFO(dbuf);
        // Deciles of the block-average metric: a game below the capture
        // rate shows two populations (repeats near 0, new pictures high).
        std::string dec = "Frame generation repeats (block average): changed deciles";
        for (int q = 1; q <= 9; ++q)
            dec += " " + std::to_string(static_cast<int>(pct(d.changedAvg, q / 10.0)));
        dec += ", peak deciles";
        for (int q = 1; q <= 9; ++q)
        {
            char t[16];
            std::snprintf(t, sizeof(t), " %.1f", pct(d.peakAvg, q / 10.0));
            dec += t;
        }
        LOG_INFO(dec);
    }
    d = DiffStats();
    s = TimingStats();
    s.windowStartUs = nowUs;
    s.lastEmitWallUs = nowUs;
}

void FrameGenStage::deactivate()
{
    if (!m_active)
        return;
    m_active = false;
    m_clock.reset();
    m_interp.reset();
    m_current = Frame();
    m_lastSrcSeq = 0;
    m_stats = TimingStats();
    m_capPeriodUs = 0;
    m_lastCapTsUs = 0;
    m_lastDistinctTsUs = 0;
    m_noiseValid = false;
    m_effFactor = 0;
    m_spans.clear();
    m_cadenceUs = 0;
    m_lastWasHeld = false;
}
