#include "FrameGenStage.h"
#include "../utils/Logger.h"

#include <cmath>
#include <cstdio>

FrameGenStage::Frame FrameGenStage::update(GLuint srcTex, uint32_t w, uint32_t h, uint64_t srcSeq,
                                           int64_t srcTsUs, int64_t nowUs, int factor,
                                           const FrameInterpolator::Options &opt)
{
    m_active = true;
    m_current.fresh = false;

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
        if (m_interp.pushFrame(srcTex, w, h))
        {
            m_clock.onRealFrame(srcTsUs, nowUs);
            m_stats.liveLagSum += static_cast<double>(nowUs - srcTsUs);
            ++m_stats.liveN;
        }
    }

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
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "Frame generation: %d frames in %.1f s (%.2f fps), interval %.2f +- %.2f ms "
                  "[%.2f..%.2f], capture->output lag %.1f ms vs %.1f ms on arrival (added %.1f ms), "
                  "dropped so far %llu",
                  s.n, (nowUs - s.windowStartUs) / 1e6, s.n * 1e6 / static_cast<double>(nowUs - s.windowStartUs),
                  mean, sd, s.ivMin, s.ivMax, outLag, liveLag, outLag - liveLag,
                  static_cast<unsigned long long>(m_clock.dropped()));
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
}
