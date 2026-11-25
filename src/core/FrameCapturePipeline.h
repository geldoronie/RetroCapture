#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "../processing/SourceCrop.h"
#include "../processing/FrameGenStage.h"
#include "../processing/UpscalerSettings.h"
#include "../renderer/glad_loader.h"

// #157 — FrameCapturePipeline: the per-frame capture -> shader -> distribute
// pipeline extracted out of Application (behavior-preserving). It does not own
// any state; it references the Application and accesses its collaborators and
// per-frame buffers directly (Application declares it a friend). Moving the
// ~1340-line render body here shrinks the Application god-object without
// changing init order, ownership, or threading.

class Application;
class PBOManager;
class AreaDownscaler;

class FrameCapturePipeline
{
public:
    explicit FrameCapturePipeline(Application &app);

    // Per-frame render + shader + capture/push. Returns true when the frame was
    // already presented (caller should skip the rest of its loop iteration).
    bool renderAndDistributeFrame();

    // #247 — release the GL objects this class owns (context current).
    void releaseGL();

    ~FrameCapturePipeline();

private:
    Application &m_app;

    // #224/#225 — the source crop for a srcW x srcH frame, as fractions of
    // the texture per side (0 = no crop); top = texture row 0.
    void getSourceCropFractions(uint32_t srcW, uint32_t srcH,
                                float &left, float &right,
                                float &top, float &bottom) const;

    // #251 — built-in upscaler stage (UpscalerStage, owned by Application).
    // Runs it when this frame's settings put it at `where`; otherwise returns
    // tex unchanged. refW/refH = native source size (2x/3x/4x refer to it).
    // Sets m_upscalerRan when it produced the texture it returns.
    GLuint runUpscaler(UpscalerSettings::Position where, GLuint tex,
                       uint32_t &w, uint32_t &h, uint32_t refW, uint32_t refH);
    // The upscale target for a refW x refH source this frame.
    void upscalerTarget(uint32_t refW, uint32_t refH, uint32_t &tw, uint32_t &th) const;
    UpscalerSettings m_upscalerSettings; // copy taken at the start of the frame
    bool m_upscalerRan = false;          // the upscaler produced this frame's image

    // #198 — identity + capture time of the pixels a readback produced.
    struct FrameMeta
    {
        uint64_t seq = 0;     // FrameProcessor frame sequence (0 = none)
        int64_t tsUs = 0;     // capture time on MediaClock
        // #247 — may this frame go to /stream / the recording? A generated
        // frame only goes to an encoder whose fps carries its phase. Kept
        // with the frame because the async readback delivers it one
        // iteration later.
        bool streamOk = true;
        bool recordOk = true;
    };
    // The async PBO readback hands back the PREVIOUS readback's pixels;
    // this remembers which frame that readback was of.
    FrameMeta m_pboInFlight;
    // Last frame pushed to each output: a captured frame is pushed once,
    // not once per render-loop iteration.
    uint64_t m_lastStreamSeq = 0;
    uint64_t m_lastRawSeq = 0;
    uint64_t m_lastRecordSeq = 0;

    // #247 — one render of a source frame through the shader chain to the
    // window and/or a subset of the outputs.
    struct PassSpec
    {
        GLuint srcTex = 0;
        uint32_t srcW = 0, srcH = 0;
        FrameMeta meta;
        bool window = true;
        bool stream = true;
        bool record = true;
        bool virtcam = true;
        bool raw = true;
        PBOManager *pbo = nullptr;          // async readback for this pass
        FrameMeta *pboInFlight = nullptr;   // ... and what it is reading
    };
    // Returns true when the frame was already presented (invalid window).
    bool renderPass(const PassSpec &spec);
    // Status / overlay counters, UI, swap and loop pacing.
    bool finishFrame();

    // #247 — frame generation: which outputs get the generated timeline
    // this iteration, and the timeline frame itself.
    struct FrameGenPlan
    {
        bool active = false;
        FrameGenStage::Frame frame;
        bool genWindow = false, genStream = false, genRecord = false, genVirtcam = false;
        bool streamPhaseOk = true, recordPhaseOk = true; // see FrameMeta
        bool liveStream = true, liveRecord = true, liveVirtcam = true;
        bool liveOutputsActive = false; // a live-pass output is running (incl. /raw)
    };
    FrameGenPlan planFrameGeneration(GLuint srcTex, uint32_t w, uint32_t h, const FrameMeta &live);
    std::unique_ptr<FrameGenStage> m_frameGen;
    std::unique_ptr<PBOManager> m_genPbo;   // the generated pass's readback
    std::unique_ptr<AreaDownscaler> m_areaDownscaler; // logical-size downscale (area average)
    int m_downscaleLogged = 0; // last downscale filter logged (1 area->nearest, 2 area, 3 nearest)
    FrameMeta m_genPboInFlight;
    bool m_frameGenVsync = false;           // display mode turned vsync on
    int m_frameGenLoggedMode = -1;

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
