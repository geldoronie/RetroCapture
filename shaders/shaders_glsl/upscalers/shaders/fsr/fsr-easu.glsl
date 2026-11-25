/*
    AMD FidelityFX Super Resolution 1.0 - EASU (Edge Adaptive Spatial Upsampling)

    Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
    THE SOFTWARE.

    Source: https://github.com/GPUOpen-Effects/FidelityFX-FSR (ffx_fsr1.h).
    GLSL fragment port based on the libretro glsl-shaders `fsr/fsr-pass0.glsl`
    (itself ported from https://www.shadertoy.com/view/stXSWB, MIT).

    RetroCapture changes: the output pixel position passed to FsrEasuF() is
    the integer pixel index (floor(gl_FragCoord)), as in the reference
    compute shader, so the con0.zw half-pixel correction lines up exactly.

    Must run with scale_type = viewport: EASU upscales the source straight to
    the output size. The input is expected in perceptual (gamma / sRGB-encoded)
    space, which is what a capture texture is, so no linearisation is done.
*/

#if defined(VERTEX)

#if __VERSION__ >= 130
#define COMPAT_VARYING out
#define COMPAT_ATTRIBUTE in
#define COMPAT_TEXTURE texture
#else
#define COMPAT_VARYING varying
#define COMPAT_ATTRIBUTE attribute
#define COMPAT_TEXTURE texture2D
#endif

#ifdef GL_ES
#define COMPAT_PRECISION mediump
#else
#define COMPAT_PRECISION
#endif

COMPAT_ATTRIBUTE vec4 VertexCoord;
COMPAT_ATTRIBUTE vec4 COLOR;
COMPAT_ATTRIBUTE vec4 TexCoord;
COMPAT_VARYING vec4 COL0;
COMPAT_VARYING vec4 TEX0;

uniform mat4 MVPMatrix;
uniform COMPAT_PRECISION int FrameDirection;
uniform COMPAT_PRECISION int FrameCount;
uniform COMPAT_PRECISION vec2 OutputSize;
uniform COMPAT_PRECISION vec2 TextureSize;
uniform COMPAT_PRECISION vec2 InputSize;

void main()
{
    gl_Position = MVPMatrix * VertexCoord;
    COL0 = COLOR;
    TEX0.xy = TexCoord.xy;
}

#elif defined(FRAGMENT)

#if __VERSION__ >= 130
#define COMPAT_VARYING in
#define COMPAT_TEXTURE texture
out vec4 FragColor;
#else
#define COMPAT_VARYING varying
#define FragColor gl_FragColor
#define COMPAT_TEXTURE texture2D
#endif

#ifdef GL_ES
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
#define COMPAT_PRECISION mediump
#else
#define COMPAT_PRECISION
#endif

uniform COMPAT_PRECISION int FrameDirection;
uniform COMPAT_PRECISION int FrameCount;
uniform COMPAT_PRECISION vec2 OutputSize;
uniform COMPAT_PRECISION vec2 TextureSize;
uniform COMPAT_PRECISION vec2 InputSize;
uniform sampler2D Texture;
COMPAT_VARYING vec4 TEX0;

#define Source Texture
// RetroCapture's shader preprocessor infers the GLSL type of OutputSize from
// how it is used; this libretro-style define pins it to vec2.
#define OUTSIZE vec4(OutputSize, 1.0 / OutputSize)

vec3 FsrEasuCF(vec2 p)
{
    return COMPAT_TEXTURE(Source, p).rgb;
}

// Constant setup (FsrEasuCon in ffx_fsr1.h).
void FsrEasuCon(out vec4 con0, out vec4 con1, out vec4 con2, out vec4 con3,
                vec2 inputViewportInPixels, vec2 inputSizeInPixels, vec2 outputSizeInPixels)
{
    // Output integer position to a pixel position in viewport.
    con0 = vec4(inputViewportInPixels.x / outputSizeInPixels.x,
                inputViewportInPixels.y / outputSizeInPixels.y,
                0.5 * inputViewportInPixels.x / outputSizeInPixels.x - 0.5,
                0.5 * inputViewportInPixels.y / outputSizeInPixels.y - 0.5);
    // Viewport pixel position to normalized image space (upper-left of 'F' tap).
    con1 = vec4(1.0, 1.0, 1.0, -1.0) / inputSizeInPixels.xyxy;
    // Centers of gather4, first offset from upper-left of 'F'.
    //      +---+---+
    //      |   |   |
    //      +--(0)--+
    //      | b | c |
    //  +---F---+---+---+
    //  | e | f | g | h |
    //  +--(1)--+--(2)--+
    //  | i | j | k | l |
    //  +---+---+---+---+
    //      | n | o |
    //      +--(3)--+
    //      |   |   |
    //      +---+---+
    con2 = vec4(-1.0, 2.0, 1.0, 2.0) / inputSizeInPixels.xyxy;
    con3 = vec4(0.0, 4.0, 0.0, 0.0) / inputSizeInPixels.xyxy;
}

// Filtering for a given tap for the scalar.
void FsrEasuTapF(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len,
                 float lob, float clp, vec3 c)
{
    // Rotate offset by direction.
    vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x)));
    // Anisotropy.
    v *= len;
    // Distance^2, limited to the window.
    float d2 = min(dot(v, v), clp);
    // Approximation of lanczos2 without sin() or rcp(), or sqrt() to get x.
    //  (25/16 * (2/5 * x^2 - 1)^2 - (25/16 - 1)) * (1/4 * x^2 - 1)^2
    float wB = 0.4 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 1.5625 * wB - 0.5625;
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

// Accumulate direction and length.
void FsrEasuSetF(inout vec2 dir, inout float len, float w,
                 float lA, float lB, float lC, float lD, float lE)
{
    // Direction is the '+' diff.
    //    a
    //  b c d
    //    e
    float lenX = max(abs(lD - lC), abs(lC - lB));
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = clamp(abs(dirX) / lenX, 0.0, 1.0);
    lenX *= lenX;
    len += lenX * w;
    float lenY = max(abs(lE - lC), abs(lC - lA));
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = clamp(abs(dirY) / lenY, 0.0, 1.0);
    lenY *= lenY;
    len += lenY * w;
}

void FsrEasuF(out vec3 pix, vec2 ip, vec4 con0, vec4 con1, vec4 con2, vec4 con3)
{
    // Position of 'f'.
    vec2 pp = ip * con0.xy + con0.zw;
    vec2 fp = floor(pp);
    pp -= fp;

    // 12-tap kernel.
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec2 p0 = fp * con1.xy + con1.zw;
    vec2 p1 = p0 + con2.xy;
    vec2 p2 = p0 + con2.zw;
    vec2 p3 = p0 + con3.xy;

    // No textureGather: expand each gather4 into 4 point-centred taps.
    // x=west y=east z=north w=south
    vec4 off = vec4(-0.5, 0.5, -0.5, 0.5) * con1.xxyy;
    vec3 bC = FsrEasuCF(p0 + off.xw); float bL = bC.g + 0.5 * (bC.r + bC.b);
    vec3 cC = FsrEasuCF(p0 + off.yw); float cL = cC.g + 0.5 * (cC.r + cC.b);
    vec3 iC = FsrEasuCF(p1 + off.xw); float iL = iC.g + 0.5 * (iC.r + iC.b);
    vec3 jC = FsrEasuCF(p1 + off.yw); float jL = jC.g + 0.5 * (jC.r + jC.b);
    vec3 fC = FsrEasuCF(p1 + off.yz); float fL = fC.g + 0.5 * (fC.r + fC.b);
    vec3 eC = FsrEasuCF(p1 + off.xz); float eL = eC.g + 0.5 * (eC.r + eC.b);
    vec3 kC = FsrEasuCF(p2 + off.xw); float kL = kC.g + 0.5 * (kC.r + kC.b);
    vec3 lC = FsrEasuCF(p2 + off.yw); float lL = lC.g + 0.5 * (lC.r + lC.b);
    vec3 hC = FsrEasuCF(p2 + off.yz); float hL = hC.g + 0.5 * (hC.r + hC.b);
    vec3 gC = FsrEasuCF(p2 + off.xz); float gL = gC.g + 0.5 * (gC.r + gC.b);
    vec3 oC = FsrEasuCF(p3 + off.yz); float oL = oC.g + 0.5 * (oC.r + oC.b);
    vec3 nC = FsrEasuCF(p3 + off.xz); float nL = nC.g + 0.5 * (nC.r + nC.b);

    // Accumulate direction/length for bilinear interpolation of the 4 nearest.
    vec2 dir = vec2(0.0);
    float len = 0.0;
    FsrEasuSetF(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    FsrEasuSetF(dir, len,        pp.x  * (1.0 - pp.y), cL, fL, gL, hL, kL);
    FsrEasuSetF(dir, len, (1.0 - pp.x) *        pp.y , fL, iL, jL, kL, nL);
    FsrEasuSetF(dir, len,        pp.x  *        pp.y , gL, jL, kL, lL, oL);

    // Normalize with approximation, and cleanup close to zero.
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = inversesqrt(dirR);
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= vec2(dirR);
    // Transform from {0 to 2} to {0 to 1} range, and shape with square.
    len = len * 0.5;
    len *= len;
    // Stretch kernel {1.0 vert|horz, to sqrt(2.0) on diagonal}.
    float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    // Anisotropic length after rotation.
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    // Window shifts from +/-{sqrt(2.0) to slightly beyond 2.0} with edge amount.
    float lob = 0.5 - 0.29 * len;
    // Distance^2 clipping point at the end of the adjustable window.
    float clp = 1.0 / lob;

    // Accumulation mixed with min/max of the 4 nearest (de-ringing).
    vec3 min4 = min(min(fC, gC), min(jC, kC));
    vec3 max4 = max(max(fC, gC), max(jC, kC));
    vec3 aC = vec3(0.0);
    float aW = 0.0;
    FsrEasuTapF(aC, aW, vec2( 0.0, -1.0) - pp, dir, len2, lob, clp, bC);
    FsrEasuTapF(aC, aW, vec2( 1.0, -1.0) - pp, dir, len2, lob, clp, cC);
    FsrEasuTapF(aC, aW, vec2(-1.0,  1.0) - pp, dir, len2, lob, clp, iC);
    FsrEasuTapF(aC, aW, vec2( 0.0,  1.0) - pp, dir, len2, lob, clp, jC);
    FsrEasuTapF(aC, aW, vec2( 0.0,  0.0) - pp, dir, len2, lob, clp, fC);
    FsrEasuTapF(aC, aW, vec2(-1.0,  0.0) - pp, dir, len2, lob, clp, eC);
    FsrEasuTapF(aC, aW, vec2( 1.0,  1.0) - pp, dir, len2, lob, clp, kC);
    FsrEasuTapF(aC, aW, vec2( 2.0,  1.0) - pp, dir, len2, lob, clp, lC);
    FsrEasuTapF(aC, aW, vec2( 2.0,  0.0) - pp, dir, len2, lob, clp, hC);
    FsrEasuTapF(aC, aW, vec2( 1.0,  0.0) - pp, dir, len2, lob, clp, gC);
    FsrEasuTapF(aC, aW, vec2( 1.0,  2.0) - pp, dir, len2, lob, clp, oC);
    FsrEasuTapF(aC, aW, vec2( 0.0,  2.0) - pp, dir, len2, lob, clp, nC);

    // Normalize and dering.
    pix = min(max4, max(min4, aC / aW));
}

void main()
{
    vec4 con0, con1, con2, con3;
    // InputSize is the real size of the input texture in this engine
    // (no power-of-two padding), so it is both the input viewport and the
    // input resource size.
    FsrEasuCon(con0, con1, con2, con3, InputSize, InputSize, OutputSize);

    vec3 c;
    FsrEasuF(c, floor(gl_FragCoord.xy), con0, con1, con2, con3);
    FragColor = vec4(c, 1.0);
}

#endif
