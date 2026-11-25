/*
    AMD FidelityFX Super Resolution 1.0 - RCAS (Robust Contrast Adaptive Sharpening)

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
    GLSL fragment port based on the libretro glsl-shaders `fsr/fsr-pass1.glsl`
    (itself ported from https://www.shadertoy.com/view/stXSWB, MIT).

    RetroCapture changes: taps are exact texel loads (texelFetch, like the
    reference FsrRcasLoadF) instead of filtered samples, and the optional
    noise-aware path (FSR_RCAS_DENOISE) is exposed as a runtime parameter.

    Runs at output size (scale_type = source, 1.0, after EASU at viewport).
    Requires GLSL 1.30+ / GLSL ES 3.00 (texelFetch).
*/

#pragma parameter FSR_SHARPENING "FSR RCAS Sharpening (stops, 0 = sharpest)" 0.2 0.0 2.0 0.05
#pragma parameter FSR_RCAS_DENOISE "FSR RCAS Noise-Aware (0 = off, 1 = on)" 0.0 0.0 1.0 1.0

#if defined(VERTEX)

#if __VERSION__ >= 130
#define COMPAT_VARYING out
#define COMPAT_ATTRIBUTE in
#else
#define COMPAT_VARYING varying
#define COMPAT_ATTRIBUTE attribute
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
out vec4 FragColor;
#else
#define COMPAT_VARYING varying
#define FragColor gl_FragColor
#endif

#ifdef GL_ES
precision highp float;
precision highp int;
#define COMPAT_PRECISION highp
#else
#define COMPAT_PRECISION
#endif

uniform COMPAT_PRECISION vec2 TextureSize;
uniform COMPAT_PRECISION vec2 InputSize;
uniform sampler2D Texture;
COMPAT_VARYING vec4 TEX0;

#ifdef PARAMETER_UNIFORM
uniform COMPAT_PRECISION float FSR_SHARPENING;
uniform COMPAT_PRECISION float FSR_RCAS_DENOISE;
#else
#define FSR_SHARPENING 0.2
#define FSR_RCAS_DENOISE 0.0
#endif

// Limit of the negative lobe (from ffx_fsr1.h).
#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

ivec2 gMaxCoord;

vec3 FsrRcasLoadF(ivec2 p)
{
    return texelFetch(Texture, clamp(p, ivec2(0), gMaxCoord), 0).rgb;
}

vec3 FsrRcasF(ivec2 ip, float con)
{
    // Minimal 3x3 neighbourhood.
    //    b
    //  d e f
    //    h
    vec3 b = FsrRcasLoadF(ip + ivec2( 0, -1));
    vec3 d = FsrRcasLoadF(ip + ivec2(-1,  0));
    vec3 e = FsrRcasLoadF(ip);
    vec3 f = FsrRcasLoadF(ip + ivec2( 1,  0));
    vec3 h = FsrRcasLoadF(ip + ivec2( 0,  1));
    // Luma times 2.
    float bL = b.g + 0.5 * (b.b + b.r);
    float dL = d.g + 0.5 * (d.b + d.r);
    float eL = e.g + 0.5 * (e.b + e.r);
    float fL = f.g + 0.5 * (f.b + f.r);
    float hL = h.g + 0.5 * (h.b + h.r);
    // Noise detection.
    float nz = 0.25 * (bL + dL + fL + hL) - eL;
    nz = clamp(abs(nz) / (max(max(bL, dL), max(eL, max(fL, hL)))
                        - min(min(bL, dL), min(eL, min(fL, hL)))), 0.0, 1.0);
    nz = 1.0 - 0.5 * nz;
    // Min and max of ring.
    vec3 mn4 = min(b, min(f, min(d, h)));
    vec3 mx4 = max(b, max(f, max(d, h)));
    // Immediate constants for peak range.
    vec2 peakC = vec2(1.0, -4.0);
    // Limiters.
    vec3 hitMin = mn4 / (4.0 * mx4);
    vec3 hitMax = (peakC.x - mx4) / (4.0 * mn4 + peakC.y);
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-FSR_RCAS_LIMIT, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * con;
    // Optional noise removal.
    if (FSR_RCAS_DENOISE > 0.5)
        lobe *= nz;
    // Resolve.
    return (lobe * (b + d + h + f) + e) / (4.0 * lobe + 1.0);
}

void main()
{
    gMaxCoord = ivec2(InputSize) - 1;
    // Sharpness is in stops: 0 = maximum, each +1 halves it (FsrRcasCon).
    float con = exp2(-FSR_SHARPENING);
    vec3 col = FsrRcasF(ivec2(gl_FragCoord.xy), con);
    FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}

#endif
