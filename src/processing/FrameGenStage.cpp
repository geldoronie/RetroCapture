#include "FrameGenStage.h"
#include "../utils/Logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
// A repeat is held at most this many capture periods (= the 4-slot cap, so
// down to a 15 fps game on 60 Hz): past it (a static scene, a menu) the
// capture goes through anyway, so the outputs never stall.
constexpr int64_t kMaxHoldPeriods = 4;
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

    if (!skipRepeats || m_interp.frameCount() < 1)
    {
        m_lastDistinctTsUs = srcTsUs;
        return true;
    }
    const FrameInterpolator::Difference diff = m_interp.compareWithLatest(srcTex, w, h);
    if (!diff.valid)
    {
        m_lastDistinctTsUs = srcTsUs;
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

    const bool held = m_capPeriodUs > 0 && srcTsUs - m_lastDistinctTsUs >= kMaxHoldPeriods * m_capPeriodUs;
    if (diff.repeat(m_noiseFloor) && !held)
    {
        ++m_repeats;
        return false;
    }
    m_lastDistinctTsUs = srcTsUs;
    return true;
}

FrameGenStage::Frame FrameGenStage::update(GLuint srcTex, uint32_t w, uint32_t h, uint64_t srcSeq,
                                           int64_t srcTsUs, int64_t nowUs, int factor, bool skipRepeats,
                                           const FrameInterpolator::Options &opt)
{
    m_active = true;
    m_current.fresh = false;
    m_interp.setMethodHint(opt.method);

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
            m_clock.onRealFrame(srcTsUs, nowUs);
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
            n = std::max(1, std::min(4, n));
        }
        factor = n;
    }
    m_effFactor = factor;

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
                  "dropped so far %llu, repeats skipped %llu",
                  s.n, (nowUs - s.windowStartUs) / 1e6, s.n * 1e6 / static_cast<double>(nowUs - s.windowStartUs),
                  mean, sd, s.ivMin, s.ivMax, outLag, liveLag, outLag - liveLag,
                  static_cast<unsigned long long>(m_clock.dropped()),
                  static_cast<unsigned long long>(m_repeats));
    LOG_INFO(buf);
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
}
