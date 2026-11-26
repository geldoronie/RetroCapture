#include "FrameGenClock.h"

namespace
{
// Plausible source frame interval: 2 ms (500 fps) .. 250 ms (4 fps).
constexpr int64_t kMinFrameUs = 2000;
constexpr int64_t kMaxFrameUs = 250000;
} // namespace

void FrameGenClock::reset()
{
    const uint64_t seq = m_seq;
    const uint64_t real = m_realEmitted, gen = m_generatedEmitted, drop = m_dropped;
    const int64_t extra = m_extraDelayUs;
    *this = FrameGenClock();
    m_extraDelayUs = extra; // a setting, not pair state
    // Sequence numbers and counters stay monotonic across resets: outputs
    // dedupe on the sequence, the overlay derives rates from the counters.
    m_seq = seq;
    m_realEmitted = real;
    m_generatedEmitted = gen;
    m_dropped = drop;
}

void FrameGenClock::onRealFrame(int64_t tsUs, int64_t nowUs)
{
    // Slots of the previous pair that never went out. B itself is still
    // owed: it goes out first on the next poll.
    m_pendingReal = false;
    if (m_haveB && !m_realBEmitted)
    {
        m_pendingReal = true;
        m_pendingRealTs = m_tsB;
    }
    if (m_hasPair && m_pairSlots > 0 && m_lastSlot < m_pairSlots - 1)
        m_dropped += static_cast<uint64_t>(m_pairSlots - 1 - m_lastSlot);

    bool pair = false;
    if (m_haveB)
    {
        const int64_t d = tsUs - m_tsB;
        if (d >= kMinFrameUs && d <= kMaxFrameUs && (m_periodUs == 0 || d <= 3 * m_periodUs))
        {
            pair = true;
            m_periodUs = m_periodUs > 0 ? (m_periodUs * 7 + d) / 8 : d;
        }
        else if (d >= kMinFrameUs && d <= kMaxFrameUs && m_periodUs > 0 && d > 3 * m_periodUs)
        {
            // A stall (or the source really slowed down): no pair this
            // time, and let the estimate follow if it keeps happening.
            m_periodUs = (m_periodUs * 3 + d) / 4;
        }
    }

    // Arrival lag, min-biased: drop quickly towards early arrivals, rise
    // slowly with late ones; a jump much larger than a frame re-latches.
    const int64_t lag = nowUs - tsUs;
    const int64_t relatch = (m_periodUs > 0 ? 2 * m_periodUs : 100000);
    if (!m_delayValid || lag - m_delayUs > relatch || m_delayUs - lag > relatch)
    {
        m_delayUs = lag;
        m_delayValid = true;
    }
    else if (lag < m_delayUs)
    {
        m_delayUs += (lag - m_delayUs) / 4;
    }
    else
    {
        m_delayUs += (lag - m_delayUs) / 32;
    }

    m_tsA = m_tsB;
    m_tsB = tsUs;
    m_haveB = true;
    m_hasPair = pair;
    m_lastSlot = 0;
    m_pairSlots = -1; // latched by the first poll of the pair
    m_realBEmitted = false;
    m_lastAlpha = 0.0f;
}

FrameGenClock::Emit FrameGenClock::makeEmit(bool generated, float alpha, int64_t tsUs,
                                            int slot, int slots)
{
    Emit e;
    if (tsUs <= m_lastEmitTs)
        return e; // never step the output timeline backwards
    e.emit = true;
    e.generated = generated;
    e.alpha = alpha;
    e.tsUs = tsUs;
    e.seq = kSeqBase + (++m_seq);
    e.slot = slot;
    e.slots = slots;
    m_lastEmitTs = tsUs;
    if (generated)
        ++m_generatedEmitted;
    else
        ++m_realEmitted;
    return e;
}

FrameGenClock::Emit FrameGenClock::poll(int64_t nowUs, int factor)
{
    if (!m_haveB)
        return Emit();

    if (m_pairSlots < 0)
        m_pairSlots = factor >= 1 ? factor : 0;

    if (m_pendingReal && factor == 1 && m_hasPair)
    {
        // Pass-through (1 slot per pair, #256): the new real frame is due
        // right away, so the owed previous one would only keep the output a
        // frame behind for good. Show the newer one.
        m_pendingReal = false;
        ++m_dropped;
    }
    if (m_pendingReal)
    {
        m_pendingReal = false;
        Emit e = makeEmit(false, 1.0f, m_pendingRealTs, m_pairSlots, m_pairSlots);
        e.previous = true;
        if (e.emit)
            return e;
    }

    if (!m_hasPair)
    {
        // No usable pair (first frame, or a discontinuity): show B as is.
        if (m_realBEmitted)
            return Emit();
        m_realBEmitted = true;
        m_lastSlot = m_pairSlots;
        return makeEmit(false, 1.0f, m_tsB, m_pairSlots, m_pairSlots);
    }

    const int64_t span = m_tsB - m_tsA;
    if (factor >= 1)
    {
        if (m_pairSlots != factor)
        {
            // Factor changed mid-pair: restart this pair's schedule.
            m_pairSlots = factor;
            m_lastSlot = m_realBEmitted ? factor : 0;
        }
        const int n = factor;
        const int64_t base = m_tsB + m_delayUs + m_extraDelayUs;
        const int64_t slack = span / (4 * n);
        int latestDue = 0;
        for (int j = m_lastSlot + 1; j <= n; ++j)
        {
            const int64_t due = base + (static_cast<int64_t>(j - 1) * span) / n - slack;
            if (nowUs >= due)
                latestDue = j;
        }
        if (latestDue == 0)
            return Emit();
        m_dropped += static_cast<uint64_t>(latestDue - m_lastSlot - 1);
        m_lastSlot = latestDue;
        const bool real = (latestDue == n);
        if (real)
            m_realBEmitted = true;
        const int64_t ts = m_tsA + (static_cast<int64_t>(latestDue) * span) / n;
        return makeEmit(!real, static_cast<float>(latestDue) / static_cast<float>(n), ts,
                        latestDue, n);
    }

    // Display mode: the phase of this very refresh.
    m_pairSlots = 0;
    const int64_t content = nowUs - m_delayUs - m_extraDelayUs - span;
    const double alpha = static_cast<double>(content - m_tsA) / static_cast<double>(span);
    if (alpha >= 0.999)
    {
        if (m_realBEmitted)
            return Emit();
        m_realBEmitted = true;
        return makeEmit(false, 1.0f, m_tsB, 0, 0);
    }
    if (alpha <= 0.0 || alpha - m_lastAlpha < 1.0 / 256.0)
        return Emit();
    m_lastAlpha = static_cast<float>(alpha);
    const int64_t ts = m_tsA + static_cast<int64_t>(alpha * static_cast<double>(span));
    return makeEmit(true, static_cast<float>(alpha), ts, 0, 0);
}
