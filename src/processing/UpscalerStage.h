#pragma once

#include "../renderer/glad_loader.h"
#include "UpscalerSettings.h"

#include <cstdint>
#include <memory>
#include <string>

class ShaderEngine;

/**
 * #251 — built-in upscaler stage (FSR 1 / NIS / Anime4K).
 *
 * Runs the bundled upscaler presets (shaders/internal/upscalers/) in a
 * dedicated ShaderEngine, independent of the user's shader preset. The
 * engine's viewport is the upscale target, so the last pass of each preset
 * renders exactly at the target size.
 *
 * Pipeline order (FrameCapturePipeline): source pre-pass / crop -> [frame
 * generation] -> process(Before) -> user shader chain -> process(After) ->
 * output geometry -> outputs. /raw reads the pre-shader source and never
 * sees this stage.
 *
 * GL thread only. Off costs nothing: no engine is created until a mode is
 * picked, and process() returns the input untouched.
 */
class UpscalerStage
{
public:
    UpscalerStage();
    ~UpscalerStage();

    // Frees the GL objects. Call with the GL context current.
    void shutdown();

    // True when the stage runs at `where` this frame.
    static bool runsAt(const UpscalerSettings &s, UpscalerSettings::Position where)
    {
        return s.active() && s.position == where;
    }

    // When runsAt(s, where): upscales `tex` (w x h) to the target and
    // returns the output texture (RGBA, same orientation as the input),
    // updating w/h to its size. Otherwise, or when the preset can't be
    // loaded, returns `tex` with w/h untouched.
    //   refW/refH: the native source size the 2x/3x/4x targets refer to.
    //   autoW/autoH: the Auto target (window / output resolution).
    //   fitAspect: keep the source aspect inside the Auto target.
    GLuint process(const UpscalerSettings &s, UpscalerSettings::Position where,
                   GLuint tex, uint32_t &w, uint32_t &h,
                   uint32_t refW, uint32_t refH,
                   uint32_t autoW, uint32_t autoH, bool fitAspect);

    // #247 — freeze the engine's temporal state (FrameCount, feedback,
    // history) for a second pass in the same iteration; see
    // ShaderEngine::setStateFrozen(). Remembered for a lazily made engine.
    void setStateFrozen(bool frozen);

    // Directory holding the internal upscaler presets.
    static std::string presetDir();

private:
    bool ensureMode(UpscalerSettings::Mode mode);
    void applySharpness(const UpscalerSettings &s);

    std::unique_ptr<ShaderEngine> m_engine;
    bool m_engineFailed = false;
    UpscalerSettings::Mode m_loadedMode = UpscalerSettings::Mode::Off;
    // A mode whose preset failed to load: not retried every frame.
    UpscalerSettings::Mode m_failedMode = UpscalerSettings::Mode::Off;
    float m_appliedSharpness = -1.0f;
    int m_logCount = 0;
    bool m_stateFrozen = false;
};
