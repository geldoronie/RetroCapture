#pragma once

#include <cstdint>
#include <vector>

#include "../processing/SourceCrop.h"

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

    // #224/#225 — the source crop for a srcW x srcH frame, as fractions of
    // the texture per side (0 = no crop); top = texture row 0.
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

    // #225 — auto-crop: a few spaced readbacks of the uncropped source.
    void runAutoCropSampling();
    struct AutoCropSampler
    {
        bool active = false;
        int threshold = 24;
        int samples = 0; // frames read back
        int found = 0;   // of those, frames with a picture (not all dark)
        uint64_t lastSeq = 0;
        uint32_t w = 0, h = 0;
        BlackBorders result;
        std::vector<uint8_t> pixels;
        static constexpr int kSamples = 6;
        static constexpr uint64_t kSeqSpacing = 5;
    };
    AutoCropSampler m_autoCrop;
};
