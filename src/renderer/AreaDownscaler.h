#pragma once

#include "glad_loader.h"
#include <cstdint>

/**
 * Area-average (box filter) downscale on the GPU: every destination pixel is
 * the coverage-weighted mean of the source texels under it, so a
 * non-integer reduction (1280x720 -> 640x480: 2 x 1.5) keeps every source
 * line's contribution instead of dropping some, like NEAREST does, and
 * averages capture noise down. Measured with --upscaler-bench on the edges
 * pattern: +1.1..1.6 dB for every upscaler downstream vs NEAREST.
 *
 * GLSL 3.30 core / ES 3.00 (texelFetch). Orientation preserved. GL thread
 * only; restores the program, VAO, texture unit 0 binding and active unit
 * it touches (the caller owns the framebuffer and viewport).
 */
class AreaDownscaler
{
public:
    AreaDownscaler() = default;
    ~AreaDownscaler() = default;
    AreaDownscaler(const AreaDownscaler &) = delete;
    AreaDownscaler &operator=(const AreaDownscaler &) = delete;

    /** Draws srcTex (sw x sh) into the bound framebuffer at dw x dh.
     *  false = unavailable (shader failed); the caller falls back. */
    bool render(GLuint srcTex, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh);

    /** Release the GL objects (context current). */
    void destroy();

private:
    bool ensure();
    GLuint m_prog = 0;
    GLuint m_vao = 0, m_vbo = 0, m_ebo = 0;
    bool m_tried = false;
    bool m_ok = false;
};
