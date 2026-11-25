#pragma once

#include <cstdint>

// #157 — FrameCapturePipeline: the per-frame capture -> shader -> distribute
// pipeline extracted out of Application (behavior-preserving). It does not own
// any state; it references the Application and accesses its collaborators and
// per-frame buffers directly (Application declares it a friend). Moving the
// ~1340-line render body here shrinks the Application god-object without
// changing init order, ownership, or threading.

class Application;

class FrameCapturePipeline
{
public:
    explicit FrameCapturePipeline(Application &app);

    // Per-frame render + shader + capture/push. Returns true when the frame was
    // already presented (caller should skip the rest of its loop iteration).
    bool renderAndDistributeFrame();

private:
    Application &m_app;

    // #224 — the source crop for a srcW x srcH frame, as fractions of the
    // frame per side (0 = no crop). Visible region = 1 - left - right etc.
    void getSourceCropFractions(uint32_t srcW, uint32_t srcH,
                                float &left, float &right,
                                float &top, float &bottom) const;

    // #198 — identity + capture time of the pixels a readback produced.
    struct FrameMeta
    {
        uint64_t seq = 0;     // FrameProcessor frame sequence (0 = none)
        int64_t tsUs = 0;     // capture time on MediaClock
    };
    // The async PBO readback hands back the PREVIOUS readback's pixels;
    // this remembers which frame that readback was of.
    FrameMeta m_pboInFlight;
    // Last frame pushed to each output: a captured frame is pushed once,
    // not once per render-loop iteration.
    uint64_t m_lastStreamSeq = 0;
    uint64_t m_lastRawSeq = 0;
    uint64_t m_lastRecordSeq = 0;
};
