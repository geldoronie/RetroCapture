#pragma once

#include "../renderer/glad_loader.h"
#include "rife/RifeBackend.h"
#include <cstdint>
#include <string>
#include <vector>

/**
 * GPU frame interpolator for frame generation (#247), fragment shaders only
 * (GLSL 3.30 core / GLSL ES 3.00), so it runs on macOS' GL 4.1 and GLES 3.
 *
 * Keeps the last two real frames (A = previous, B = latest) and synthesises
 * the picture at any phase t in (0, 1) between them:
 *
 *  1. Per real frame: copy into a history slot (RGBA8, source size) and build
 *     a luma pyramid at a downscaled working size (finest level <= 640 px
 *     wide: 1/4 of 1080p, 1/2 of 720p).
 *  2. Per pair (once, on first use): coarse-to-fine *bilateral* block
 *     matching. For each pixel p of the mid-time plane it looks for the
 *     vector v minimising SAD(A(p - v/2), B(p + v/2)) over a 3x3 window:
 *     full +-4 px search on the coarsest level, then at every finer level
 *     the upscaled coarse vector, its 4 coarse neighbours and the zero
 *     vector as candidates, followed by +-1 and +-0.5 px refinement. A
 *     smoothness penalty keeps flat areas on the coarse prediction; a small
 *     bias keeps static areas on zero motion. A final 3x3 "vector median by
 *     cost" pass removes isolated outliers. Output per pixel (RGBA16F):
 *     vector, its SAD (confidence) and the zero-vector SAD (static test).
 *  3. Per output frame: bidirectional warp of A and B to time t along the
 *     mid-time vector field (two fixed-point steps put the vector on the
 *     trajectory through the output pixel for t != 0.5), with an
 *     edge-aware pick among the 4 nearest flow texels so motion boundaries
 *     do not smear, and a confidence blend that falls back to the plain
 *     A/B blend where matching failed (occlusions, new content). Static
 *     regions (HUD) can be pinned to the plain blend.
 *
 * Method RIFE (#248) hands the pair to a neural interpolator (RIFE 4.x on
 * ncnn + Vulkan, see rife/RifeBackend.h) running on its own thread. Its
 * result replaces step 3 when it is ready in time; otherwise (still loading,
 * no Vulkan device, inference late) the frame is synthesised with Motion,
 * so the output never stalls on the network.
 *
 * All GL state the passes touch (framebuffer, viewport, program, VAO,
 * texture bindings on units 0-3, active unit) is restored. GL thread only.
 */
class FrameInterpolator
{
public:
    enum class Method
    {
        Blend,
        Motion,
        RIFE, // neural (#248); falls back to Motion frame by frame
    };
    struct Options
    {
        Method method = Method::Motion;
        bool protectStatic = true;
        bool edgeAwarePick = true; // step 3 candidate pick (bench A/B switch)
        // RIFE: how long render() may wait for the worker's result before
        // falling back to Motion (< 0 = no limit, bench only).
        int rifeWaitMs = 4;
    };
    // Wall time of each stage, measured with glFinish() around it. Only
    // filled while profiling is on (it stalls the pipeline): bench use.
    struct Timings
    {
        double pushMs = 0.0;  // copy + luma pyramid (per real frame)
        double flowMs = 0.0;  // motion search (per pair)
        double warpMs = 0.0;  // synthesis (per generated frame)
        double rifeMs = 0.0;  // RIFE: wait for the worker + texture upload
    };

    FrameInterpolator() = default;
    ~FrameInterpolator() = default;
    FrameInterpolator(const FrameInterpolator &) = delete;
    FrameInterpolator &operator=(const FrameInterpolator &) = delete;

    /** Compiles the programs on first call; false = unusable on this GL. */
    bool isSupported();
    /** Why isSupported() is false (empty when supported). */
    const std::string &unsupportedReason() const { return m_failReason; }
    /** The Motion method needs float render targets; false = Blend only. */
    bool motionSupported() const { return m_motionOk; }

    /**
     * The method the next frames will be rendered with. RIFE starts the
     * backend (async) and makes pushFrame() feed it; other methods stop
     * feeding it (the model stays loaded until destroy()).
     */
    void setMethodHint(Method m);
    RifeBackend &rife() { return m_rife; }
    const RifeBackend &rife() const { return m_rife; }
    /** The last render() output came from RIFE (not the Motion fallback). */
    bool lastWasRife() const { return m_lastRife; }

    /** Store a new real frame (copied; srcTex may be reused afterwards). */
    bool pushFrame(GLuint srcTex, uint32_t w, uint32_t h);

    /**
     * Repeat detection (#256): how srcTex differs from the latest real frame
     * (B). A block-wise luma difference on a small grid is read back; the
     * noise floor is the median block difference, so an analog capture's
     * noise does not count as change. Synchronous (one small readback).
     */
    struct Difference
    {
        bool valid = false;     // false = no history / different size
        float noise = 0.0f;     // median block mean |dLuma| (0..1)
        float peak = 0.0f;      // largest block mean |dLuma|
        int changedBlocks = 0;  // blocks clearly above the noise floor
        int blocks = 0;
        /**
         * The game did not draw a new picture. noiseFloor = the source's
         * usual frame-to-frame noise (tracked by the caller): a uniform
         * change (fade) lifts the median above it and is not a repeat.
         */
        bool repeat(float noiseFloor) const
        {
            return valid && changedBlocks < 2 && peak - noise < 24.0f / 255.0f &&
                   noise <= noiseFloor + 2.0f / 255.0f;
        }
    };
    Difference compareWithLatest(GLuint srcTex, uint32_t w, uint32_t h);

    /** Real frames held: 0, 1 or 2. */
    int frameCount() const { return m_count; }
    /** Latest real frame (B) / the one before it (A); 0 if absent. */
    GLuint latestTexture() const;
    GLuint previousTexture() const;
    uint32_t width() const { return m_w; }
    uint32_t height() const { return m_h; }

    /**
     * Synthesise the frame at phase t (0 = A, 1 = B). Returns the output
     * texture (RGBA8, source size; valid until the next render()), or 0.
     */
    GLuint render(float t, const Options &opt);

    /** Forget the history (keeps GL objects). */
    void reset();
    /** Release every GL object (requires a current context). */
    void destroy();

    /** Presentation filter of the textures handed out (mirrors the source). */
    void setFilterLinear(bool linear);

    void setProfiling(bool on) { m_profile = on; }
    const Timings &timings() const { return m_timings; }

    // Working-size geometry, for logs and the cost report.
    uint32_t flowWidth() const { return m_levels.empty() ? 0 : m_levels[0].w; }
    uint32_t flowHeight() const { return m_levels.empty() ? 0 : m_levels[0].h; }
    int pyramidLevels() const { return static_cast<int>(m_levels.size()); }

private:
    struct Target
    {
        GLuint tex = 0;
        GLuint fbo = 0;
        uint32_t w = 0;
        uint32_t h = 0;
    };
    struct LevelSize
    {
        uint32_t w = 0;
        uint32_t h = 0;
    };
    struct Slot
    {
        Target color;
        std::vector<Target> luma;
    };

    bool ensurePrograms();
    bool ensureTargets(uint32_t w, uint32_t h);
    void releaseTargets();
    bool makeTarget(Target &t, uint32_t w, uint32_t h, GLint internalFmt, GLenum fmt,
                    GLenum type, bool linear);
    void freeTarget(Target &t);
    GLuint buildProgram(const std::string &fragBody, const char *name);
    void drawTo(const Target &t);
    void bindTex(int unit, GLuint tex, bool linear);
    void buildPyramid(Slot &s);
    void computeFlow();
    void restoreFilters();

    // Programs.
    GLuint m_progCopy = 0;
    GLuint m_progLuma = 0;
    GLuint m_progDown = 0;
    GLuint m_progSearch = 0;
    GLuint m_progConsensus = 0;
    GLuint m_progWarp = 0;
    GLuint m_progDiff = 0;
    GLuint m_vao = 0, m_vbo = 0, m_ebo = 0;
    bool m_programsTried = false;
    bool m_programsOk = false;
    bool m_motionOk = false;
    std::string m_failReason;
    bool m_isES = false;

    // Geometry.
    uint32_t m_w = 0, m_h = 0;
    int m_scale = 1; // source px per level-0 px
    std::vector<LevelSize> m_levels;

    // Two history slots and the output.
    Slot m_slots[2];
    int m_latest = -1; // slot index of B
    int m_count = 0;
    Target m_out;
    std::vector<Target> m_flow; // per level
    Target m_flowFinal;         // consensus output (finest level)
    Target m_diff;              // repeat detection grid (#256)
    std::vector<uint8_t> m_diffPixels;
    bool m_flowValid = false;

    RifeBackend m_rife;
    bool m_rifeWanted = false;
    bool m_lastRife = false;

    bool m_filterLinear = false;
    bool m_profile = false;
    Timings m_timings;
};
