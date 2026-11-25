#pragma once

#include "../renderer/glad_loader.h"
#include <cstdint>

/**
 * Temporal denoise for noisy (typically analog) sources (#268).
 *
 * A motion-adaptive recursive filter: out = mix(current, history, k). k is
 * high where the picture barely changed (noise) and falls to 0 where it
 * really changed (motion), so still areas average over many frames while
 * moving ones stay sharp. Motion is measured on the luma difference of a
 * 3x3 box around the pixel (noise divided by ~3, real motion kept); a large
 * single-pixel difference also cuts k so edges do not ghost.
 *
 * It runs once per captured frame (FrameProcessor), after the source
 * pre-pass, so the shader chain, frame generation, the upscaler and every
 * output (including /raw) see the cleaned picture. Only the current and
 * past frames are used: no added latency.
 *
 * The history is RGBA16F where the GL can render to it (no banding from the
 * recursion), RGBA8 otherwise. GL thread only; bindings are restored.
 */
class TemporalDenoiser
{
public:
    TemporalDenoiser() = default;
    ~TemporalDenoiser() = default;
    TemporalDenoiser(const TemporalDenoiser &) = delete;
    TemporalDenoiser &operator=(const TemporalDenoiser &) = delete;

    /**
     * Filter one new frame (srcTex, w x h) with strength 0..1 and advance
     * the history. Returns the output texture (same size and orientation),
     * or 0 on failure (the caller keeps the unfiltered texture).
     */
    GLuint apply(GLuint srcTex, uint32_t w, uint32_t h, float strength);
    /**
     * How strength maps to the filter (levels are 8-bit):
     *  - box-luma change below t0 = full averaging, above t1 = none, with
     *    t1 = t1Base + t1Slope * strength and t0 = t0Ratio * t1;
     *  - a per-pixel change between pixLo * t1 and pixHi * t1 also stops it;
     *  - still areas keep kBase + kSlope * strength of the history.
     * The defaults are the tuned ones; the bench can override them.
     */
    struct Tuning
    {
        float t1Base = 4.0f, t1Slope = 15.0f, t0Ratio = 0.35f;
        float pixLo = 2.5f, pixHi = 5.0f;
        float kBase = 0.5f, kSlope = 0.4f;
        float line = 0.6f; // row / column sums vs the box (noise is higher there)
    };
    void setTuning(const Tuning &t) { m_tuning = t; }

    /** Forget the history: the next frame passes through and restarts it. */
    void reset() { m_valid = false; }
    /** Filtering for the output textures (mirrors FrameProcessor's). */
    void setFilterLinear(bool linear);
    /** Release every GL object (requires a current context). */
    void destroy();

private:
    bool ensureProgram();
    bool ensureTargets(uint32_t w, uint32_t h);
    bool makeTarget(int i, uint32_t w, uint32_t h, bool half);

    GLuint m_program = 0;
    GLuint m_vao = 0, m_vbo = 0, m_ebo = 0;
    GLuint m_fbo[2] = {0, 0};
    GLuint m_tex[2] = {0, 0};
    uint32_t m_w = 0, m_h = 0;
    int m_last = 0;       // index of the texture holding the last output
    bool m_valid = false; // m_tex[m_last] is a usable history
    bool m_programFailed = false;
    bool m_filterLinear = false;
    bool m_halfFailed = false; // RGBA16F not renderable here: RGBA8

    GLint m_locCur = -1, m_locPrev = -1, m_locTexel = -1;
    GLint m_locT0 = -1, m_locT1 = -1, m_locKMax = -1, m_locFirst = -1;
    GLint m_locPixLo = -1, m_locPixHi = -1, m_locLine = -1;
    Tuning m_tuning;
};
