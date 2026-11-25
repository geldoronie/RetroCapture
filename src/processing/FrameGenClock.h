#pragma once

#include <cstdint>

/**
 * Output clock for frame generation (#247). Pure CPU logic, no GL.
 *
 * The render loop tells it when a real (captured) frame arrives and asks it,
 * once per iteration, whether a frame is due on the output timeline. The
 * timeline runs one source frame behind capture: interpolating between the
 * last two real frames A and B needs B, so it is the 1-frame look-ahead.
 *
 * Timing is derived from the capture timestamps, not from arrival times, so
 * capture jitter does not leak into the output pacing:
 *  - span   = tsB - tsA (the actual interval of the current pair);
 *  - delay  = typical arrival lag (arrival - capture timestamp), tracked
 *             min-biased so a late frame does not push the schedule back.
 *
 * Fixed-multiple mode (factor N = 2..4): slot j (1..N) of the pair has
 * phase j/N, timestamp tsA + j/N * span and is due at
 *     tsB + delay + (j-1)/N * span   (minus a quarter slot of slack),
 * so slot 1 is due as soon as B arrives and slot N (B itself) (N-1)/N of a
 * frame later. A loop that falls behind presents the latest due slot and
 * counts the skipped ones as dropped, like bob deinterlacing (#229).
 *
 * Display mode (factor 0): every poll is an output tick (one per vsync
 * refresh); its phase is where "now - delay - span" falls between tsA and
 * tsB, so each refresh shows the picture of its own instant.
 *
 * Real frames win over generated ones: if the next frame arrives before B
 * itself went out (a loop running close to the source period), B is still
 * presented first, then the new pair's schedule resumes.
 *
 * A gap that is not a plausible frame interval (source paused, restarted,
 * reconfigured) breaks the pair: B is presented as is, no interpolation.
 */
class FrameGenClock
{
public:
    struct Emit
    {
        bool emit = false;      // a new output frame is due this iteration
        bool generated = false; // false = the real frame B
        float alpha = 1.0f;     // phase between A (0) and B (1)
        int64_t tsUs = 0;       // timestamp on the capture timeline
        uint64_t seq = 0;       // output sequence number (unique per frame)
        int slot = 0;           // fixed-multiple mode: slot index 1..N
        int slots = 0;          // ... and N (0 in display mode)
        bool previous = false;  // the real frame is the PREVIOUS one (A):
                                // a late B handed out after C arrived
    };

    void reset();

    /** A new real frame arrived (capture timestamp, MediaClock now). */
    void onRealFrame(int64_t tsUs, int64_t nowUs);

    /** Is an output frame due? factor = 2..4 (fixed multiple), 0 = display. */
    Emit poll(int64_t nowUs, int factor);

    /**
     * Extra presentation delay on top of the arrival lag (#248): an
     * asynchronous synthesiser (RIFE) gets this much time after B arrives
     * before the pair's first slot is due. Shifts the whole timeline (both
     * modes); 0 = the phase-1 schedule.
     */
    void setExtraDelayUs(int64_t us) { m_extraDelayUs = us > 0 ? us : 0; }
    int64_t extraDelayUs() const { return m_extraDelayUs; }

    bool hasPair() const { return m_hasPair; }
    int64_t spanUs() const { return m_tsB - m_tsA; }
    int64_t delayUs() const { return m_delayUs; }
    int64_t periodUs() const { return m_periodUs; }

    // Monotonic counters for the FPS overlay / API.
    uint64_t realEmitted() const { return m_realEmitted; }
    uint64_t generatedEmitted() const { return m_generatedEmitted; }
    uint64_t dropped() const { return m_dropped; }

    // Output sequence numbers live in their own range so they can never
    // collide with FrameProcessor sequence numbers in the per-output dedupe.
    static constexpr uint64_t kSeqBase = 1ULL << 62;

private:
    Emit makeEmit(bool generated, float alpha, int64_t tsUs, int slot, int slots);

    bool m_haveB = false;
    bool m_hasPair = false;
    int64_t m_tsA = 0;
    int64_t m_tsB = 0;
    int64_t m_periodUs = 0;
    int64_t m_delayUs = 0;
    bool m_delayValid = false;
    int64_t m_extraDelayUs = 0;

    // Progress through the current pair.
    int m_lastSlot = 0;          // fixed mode: last slot emitted (0 = none)
    bool m_realBEmitted = false; // B itself went out
    float m_lastAlpha = 0.0f;    // display mode: last phase emitted
    int m_pairSlots = 0;         // N the current pair is being scheduled with
    bool m_pendingReal = false;  // previous B still owed (see above)
    int64_t m_pendingRealTs = 0;

    int64_t m_lastEmitTs = 0;
    uint64_t m_seq = 0;

    uint64_t m_realEmitted = 0;
    uint64_t m_generatedEmitted = 0;
    uint64_t m_dropped = 0;
};
