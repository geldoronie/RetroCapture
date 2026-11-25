#pragma once

#include "../renderer/glad_loader.h"
#include <cstdint>

/**
 * Source colour range (#228). Describes the quantization of the incoming
 * signal; the pipeline always produces full-range RGB from it.
 *  - Auto:    use what the capture backend reports (V4L2 quantization),
 *             otherwise the historical behaviour: Y'CbCr decoded as
 *             limited range, RGB taken as full range.
 *  - Limited: the source uses 16-235 levels.
 *  - Full:    the source uses 0-255 levels.
 */
enum class SourceColorRange : int
{
    Auto = 0,
    Limited = 1,
    Full = 2,
};

/**
 * Per-frame source adjustments applied before the shader chain (#228), so
 * the shader chain and every output (window, stream, recording, virtual
 * camera, /raw) see the corrected image. Defaults are the identity.
 */
struct SourceAdjustments
{
    SourceColorRange colorRange = SourceColorRange::Auto;
    float saturation = 1.0f; // 0 = greyscale, 1 = unchanged
    float gamma = 1.0f;      // out = in^(1/gamma); >1 brightens mid-tones
};

/**
 * One GPU pass over the uploaded capture texture. Owns its program, quad
 * and output FBO. Must only be used on the GL thread.
 */
class SourcePrePass
{
public:
    struct Params
    {
        bool expandLimitedRange = false; // RGB 16-235 -> 0-255
        float saturation = 1.0f;
        float gamma = 1.0f;
    };

    SourcePrePass() = default;
    ~SourcePrePass();

    SourcePrePass(const SourcePrePass &) = delete;
    SourcePrePass &operator=(const SourcePrePass &) = delete;

    /** True when the parameters would change the image. */
    static bool isActive(const Params &p);

    /**
     * Render `srcTex` (w x h) through the pass. Returns the output texture
     * (same size and orientation as the input), or 0 on failure — the
     * caller then keeps using the unprocessed texture. GL bindings the
     * pass touches (framebuffer, viewport, program, texture) are restored.
     */
    GLuint apply(GLuint srcTex, uint32_t w, uint32_t h, const Params &p);

    /** Filtering for the output texture (mirrors FrameProcessor's). */
    void setFilterLinear(bool linear);

    /** Release every GL object (requires a current context). */
    void destroy();

private:
    bool ensureProgram();
    bool ensureTarget(uint32_t w, uint32_t h);

    GLuint m_program = 0;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    GLuint m_ebo = 0;
    GLuint m_fbo = 0;
    GLuint m_outTex = 0;
    uint32_t m_outW = 0;
    uint32_t m_outH = 0;
    bool m_programFailed = false;
    bool m_filterLinear = false;

    GLint m_locTex = -1;
    GLint m_locExpand = -1;
    GLint m_locSaturation = -1;
    GLint m_locInvGamma = -1;
};
