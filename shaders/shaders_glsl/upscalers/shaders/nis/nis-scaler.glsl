/*
    The MIT License(MIT)

    Copyright(c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy of
    this software and associated documentation files(the "Software"), to deal in
    the Software without restriction, including without limitation the rights to
    use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of
    the Software, and to permit persons to whom the Software is furnished to do so,
    subject to the following conditions :

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
    FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
    COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
    IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
    CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

/*
    NVIDIA Image Scaling (NIS) v1.0.3 - NVScaler (spatial upscale + adaptive
    sharpening), SDR path.

    Source: https://github.com/NVIDIAGameWorks/NVIDIAImageScaling
    (NIS/NIS_Scaler.h, NIS/NIS_Config.h). Fragment-shader formulation taken
    from the libretro slang-shaders `edge-smoothing/nis/shaders/nis-scaler.slang`
    port (same NVIDIA MIT license), converted to the libretro GLSL pass format:
      - NISLuma (alias of pass 0): source-resolution luma, read with texelFetch;
      - NISCoef (alias of pass 1): 6x128 coefficient LUT, read with texelFetch;
      - OrigTexture: the original input, bilinear-sampled for chroma.
    Per-output-pixel math is unchanged from the reference.

    Must run with scale_type = viewport. Input is expected in perceptual
    (gamma / sRGB-encoded) space, like the SDK's SDR path. Requires GLSL 1.30+
    / GLSL ES 3.00 (texelFetch, textureLod).
*/

#pragma parameter NIS_SHARPNESS "NIS Sharpness" 0.5 0.0 1.0 0.05

#if defined(VERTEX)

#if __VERSION__ >= 130
#define COMPAT_VARYING out
#define COMPAT_ATTRIBUTE in
#else
#define COMPAT_VARYING varying
#define COMPAT_ATTRIBUTE attribute
#endif

COMPAT_ATTRIBUTE vec4 VertexCoord;
COMPAT_ATTRIBUTE vec4 TexCoord;
COMPAT_VARYING vec4 TEX0;
uniform mat4 MVPMatrix;

void main()
{
    gl_Position = MVPMatrix * VertexCoord;
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

uniform COMPAT_PRECISION vec2 OutputSize;
// RetroCapture's shader preprocessor infers the GLSL type of OutputSize from
// how it is used; this libretro-style define pins it to vec2.
#define OUTSIZE vec4(OutputSize, 1.0 / OutputSize)
uniform COMPAT_PRECISION vec4 NISLumaSize;
uniform sampler2D NISLuma;      // luma prepass (luma in .r), source resolution
uniform sampler2D NISCoef;      // coefficient LUT (6x128)
uniform sampler2D OrigTexture;  // original input RGB (for chroma)
COMPAT_VARYING vec4 TEX0;

#ifdef PARAMETER_UNIFORM
uniform COMPAT_PRECISION float NIS_SHARPNESS;
#else
#define NIS_SHARPNESS 0.5
#endif

#define kPhaseCount 64
#define kSupportSize 6
float cScaler(int phase, int tap) { return texelFetch(NISCoef, ivec2(tap, phase), 0).r; }
float cUSM(int phase, int tap)    { return texelFetch(NISCoef, ivec2(tap, phase + 64), 0).r; }

// --- Config constants derived in-shader from the sharpness slider (SDR path).
// Mirrors NVScalerUpdateConfig() in NIS_Config.h. ---
float kDetectRatio, kDetectThres, kMinContrastRatio, kRatioNorm;
float kContrastBoost, kEps, kSharpStartY, kSharpScaleY;
float kSharpStrengthMin, kSharpStrengthScale, kSharpLimitMin, kSharpLimitScale;

void NISConfigSDR(float sharpness) {
    sharpness = clamp(sharpness, 0.0, 1.0);
    float slider = sharpness - 0.5;
    float MaxScale   = (slider >= 0.0) ? 1.25 : 1.75;
    float MinScale   = (slider >= 0.0) ? 1.25 : 1.0;
    float LimitScale = (slider >= 0.0) ? 1.25 : 1.0;

    kDetectRatio      = 2.0 * 1127.0 / 1024.0;
    kDetectThres      = 64.0 / 1024.0;
    kMinContrastRatio = 2.0;
    float kMaxContrastRatio = 10.0;

    kSharpStartY = 0.45;
    float kSharpEndY = 0.9;
    kSharpStrengthMin   = max(0.0, 0.4 + slider * MinScale * 1.2);
    float kSharpStrengthMax = 1.6 + slider * MaxScale * 1.8;
    kSharpLimitMin      = max(0.1, 0.14 + slider * LimitScale * 0.32);
    float kSharpLimitMax    = 0.5 + slider * LimitScale * 0.6;

    kRatioNorm          = 1.0 / (kMaxContrastRatio - kMinContrastRatio);
    kSharpScaleY        = 1.0 / (kSharpEndY - kSharpStartY);
    kSharpStrengthScale = kSharpStrengthMax - kSharpStrengthMin;
    kSharpLimitScale    = kSharpLimitMax - kSharpLimitMin;
    kContrastBoost      = 1.0;
    kEps                = 1.0 / 255.0;
}

float getY(vec3 rgb) {
    return 0.2126 * rgb.r + 0.7152 * rgb.g + 0.0722 * rgb.b;
}

// 6x6 luma support, global so helpers can index it without copying.
float P[36];
#define PIX(i,j) P[(i)*6 + (j)]

// Sobel-style 3x3 edge classifier at base offset (bi,bj) into P. Mirrors GetEdgeMap().
vec4 GetEdgeMap(int bi, int bj) {
    float g_0   = abs(PIX(0+bi, 0+bj) + PIX(0+bi, 1+bj) + PIX(0+bi, 2+bj) - PIX(2+bi, 0+bj) - PIX(2+bi, 1+bj) - PIX(2+bi, 2+bj));
    float g_45  = abs(PIX(1+bi, 0+bj) + PIX(0+bi, 0+bj) + PIX(0+bi, 1+bj) - PIX(2+bi, 1+bj) - PIX(2+bi, 2+bj) - PIX(1+bi, 2+bj));
    float g_90  = abs(PIX(0+bi, 0+bj) + PIX(1+bi, 0+bj) + PIX(2+bi, 0+bj) - PIX(0+bi, 2+bj) - PIX(1+bi, 2+bj) - PIX(2+bi, 2+bj));
    float g_135 = abs(PIX(1+bi, 0+bj) + PIX(2+bi, 0+bj) + PIX(2+bi, 1+bj) - PIX(0+bi, 1+bj) - PIX(0+bi, 2+bj) - PIX(1+bi, 2+bj));

    float g_0_90_max   = max(g_0, g_90);
    float g_0_90_min   = min(g_0, g_90);
    float g_45_135_max = max(g_45, g_135);
    float g_45_135_min = min(g_45, g_135);

    if (g_0_90_max + g_45_135_max == 0.0)
        return vec4(0.0);

    float e_0_90   = min(g_0_90_max / (g_0_90_max + g_45_135_max), 1.0);
    float e_45_135 = 1.0 - e_0_90;

    bool c_0_90    = (g_0_90_max > (g_0_90_min * kDetectRatio)) && (g_0_90_max > kDetectThres) && (g_0_90_max > g_45_135_min);
    bool c_45_135  = (g_45_135_max > (g_45_135_min * kDetectRatio)) && (g_45_135_max > kDetectThres) && (g_45_135_max > g_0_90_min);
    bool c_g_0_90  = (g_0_90_max == g_0);
    bool c_g_45_135= (g_45_135_max == g_45);

    float f_e_0_90   = (c_0_90 && c_45_135) ? e_0_90   : 1.0;
    float f_e_45_135 = (c_0_90 && c_45_135) ? e_45_135 : 1.0;

    float weight_0   = (c_0_90 &&  c_g_0_90)   ? f_e_0_90   : 0.0;
    float weight_90  = (c_0_90 && !c_g_0_90)   ? f_e_0_90   : 0.0;
    float weight_45  = (c_45_135 &&  c_g_45_135)? f_e_45_135 : 0.0;
    float weight_135 = (c_45_135 && !c_g_45_135)? f_e_45_135 : 0.0;

    return vec4(weight_0, weight_90, weight_45, weight_135);
}

vec4 GetInterpEdgeMap(vec4 e00, vec4 e01, vec4 e10, vec4 e11, float fx, float fy) {
    vec4 h0 = mix(e00, e01, fx);
    vec4 h1 = mix(e10, e11, fx);
    return mix(h0, h1, fy);
}

float CalcLTI(float p0, float p1, float p2, float p3, float p4, float p5, int phase_index) {
    bool selector = (phase_index <= kPhaseCount / 2);
    float sel = selector ? p0 : p3;
    float a_min = min(min(p1, p2), sel);
    float a_max = max(max(p1, p2), sel);
    sel = selector ? p2 : p5;
    float b_min = min(min(p3, p4), sel);
    float b_max = max(max(p3, p4), sel);

    float a_cont = a_max - a_min;
    float b_cont = b_max - b_min;
    float cont_ratio = max(a_cont, b_cont) / (min(a_cont, b_cont) + kEps);
    return (1.0 - clamp((cont_ratio - kMinContrastRatio) * kRatioNorm, 0.0, 1.0)) * kContrastBoost;
}

float EvalPoly6(float pxl[6], int phase_int) {
    phase_int = clamp(phase_int, 0, 63);
    float y = 0.0;
    for (int i = 0; i < 6; ++i) y += cScaler(phase_int, i) * pxl[i];
    float y_usm = 0.0;
    for (int i = 0; i < 6; ++i) y_usm += cUSM(phase_int, i) * pxl[i];

    float y_scale = 1.0 - clamp((y - kSharpStartY) * kSharpScaleY, 0.0, 1.0);
    float y_sharpness = y_scale * kSharpStrengthScale + kSharpStrengthMin;
    y_usm *= y_sharpness;
    float y_sharpness_limit = (y_scale * kSharpLimitScale + kSharpLimitMin) * y;
    y_usm = min(y_sharpness_limit, max(-y_sharpness_limit, y_usm));
    y_usm *= CalcLTI(pxl[0], pxl[1], pxl[2], pxl[3], pxl[4], pxl[5], phase_int);
    return y + y_usm;
}

float FilterNormal(int phase_x, int phase_y) {
    phase_x = clamp(phase_x, 0, 63);
    phase_y = clamp(phase_y, 0, 63);
    float h_acc = 0.0;
    for (int j = 0; j < 6; ++j) {
        float v_acc = 0.0;
        for (int i = 0; i < 6; ++i)
            v_acc += PIX(i, j) * cScaler(phase_y, i);
        h_acc += v_acc * cScaler(phase_x, j);
    }
    return h_acc;
}

float AddDirFilters(float fx, float fy, int fx_int, int fy_int, vec4 w) {
    float f = 0.0;
    if (w.x > 0.0) {
        float interp0Deg[6];
        for (int i = 0; i < 6; ++i) interp0Deg[i] = mix(PIX(i, 2), PIX(i, 3), fx);
        f += EvalPoly6(interp0Deg, fy_int) * w.x;
    }
    if (w.y > 0.0) {
        float interp90Deg[6];
        for (int i = 0; i < 6; ++i) interp90Deg[i] = mix(PIX(2, i), PIX(3, i), fy);
        f += EvalPoly6(interp90Deg, fx_int) * w.y;
    }
    if (w.z > 0.0) {
        float pphase_b45 = 0.5 + 0.5 * (fx - fy);
        float t[7];
        t[1] = mix(PIX(2, 1), PIX(1, 2), pphase_b45);
        t[3] = mix(PIX(3, 2), PIX(2, 3), pphase_b45);
        t[5] = mix(PIX(4, 3), PIX(3, 4), pphase_b45);
        {
            pphase_b45 = pphase_b45 - 0.5;
            float a = (pphase_b45 >= 0.0) ? PIX(0, 2) : PIX(2, 0);
            float b = (pphase_b45 >= 0.0) ? PIX(1, 3) : PIX(3, 1);
            float c = (pphase_b45 >= 0.0) ? PIX(2, 4) : PIX(4, 2);
            float d = (pphase_b45 >= 0.0) ? PIX(3, 5) : PIX(5, 3);
            t[0] = mix(PIX(1, 1), a, abs(pphase_b45));
            t[2] = mix(PIX(2, 2), b, abs(pphase_b45));
            t[4] = mix(PIX(3, 3), c, abs(pphase_b45));
            t[6] = mix(PIX(4, 4), d, abs(pphase_b45));
        }
        float interp45Deg[6];
        float pphase_p45 = fx + fy;
        if (pphase_p45 >= 1.0) {
            for (int i = 0; i < 6; ++i) interp45Deg[i] = t[i + 1];
            pphase_p45 -= 1.0;
        } else {
            for (int i = 0; i < 6; ++i) interp45Deg[i] = t[i];
        }
        f += EvalPoly6(interp45Deg, int(pphase_p45 * 64.0)) * w.z;
    }
    if (w.w > 0.0) {
        float pphase_b135 = 0.5 * (fx + fy);
        float t[7];
        t[1] = mix(PIX(3, 1), PIX(4, 2), pphase_b135);
        t[3] = mix(PIX(2, 2), PIX(3, 3), pphase_b135);
        t[5] = mix(PIX(1, 3), PIX(2, 4), pphase_b135);
        {
            pphase_b135 = pphase_b135 - 0.5;
            float a = (pphase_b135 >= 0.0) ? PIX(5, 2) : PIX(3, 0);
            float b = (pphase_b135 >= 0.0) ? PIX(4, 3) : PIX(2, 1);
            float c = (pphase_b135 >= 0.0) ? PIX(3, 4) : PIX(1, 2);
            float d = (pphase_b135 >= 0.0) ? PIX(2, 5) : PIX(0, 3);
            t[0] = mix(PIX(4, 1), a, abs(pphase_b135));
            t[2] = mix(PIX(3, 2), b, abs(pphase_b135));
            t[4] = mix(PIX(2, 3), c, abs(pphase_b135));
            t[6] = mix(PIX(1, 4), d, abs(pphase_b135));
        }
        float interp135Deg[6];
        float pphase_p135 = 1.0 + (fx - fy);
        if (pphase_p135 >= 1.0) {
            for (int i = 0; i < 6; ++i) interp135Deg[i] = t[i + 1];
            pphase_p135 -= 1.0;
        } else {
            for (int i = 0; i < 6; ++i) interp135Deg[i] = t[i];
        }
        f += EvalPoly6(interp135Deg, int(pphase_p135 * 64.0)) * w.w;
    }
    return f;
}

void main() {
    NISConfigSDR(NIS_SHARPNESS);

    // Output pixel (viewport-res pass) -> source coordinate.
    vec4 srcSize = NISLumaSize;
    ivec2 dst = ivec2(gl_FragCoord.xy);
    float kScaleX = srcSize.x / OutputSize.x; // src/out
    float kScaleY = srcSize.y / OutputSize.y;

    float srcX = (0.5 + float(dst.x)) * kScaleX - 0.5;
    float srcY = (0.5 + float(dst.y)) * kScaleY - 0.5;
    int ix0 = int(floor(srcX));
    int iy0 = int(floor(srcY));
    float fx = srcX - floor(srcX);
    float fy = srcY - floor(srcY);
    int fx_int = int(fx * float(kPhaseCount));
    int fy_int = int(fy * float(kPhaseCount));

    // 6x6 luma support, read straight from the luma prepass.
    ivec2 maxc = ivec2(srcSize.xy) - 1;
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) {
            ivec2 sp = clamp(ivec2(ix0 + (j - 2), iy0 + (i - 2)), ivec2(0), maxc);
            PIX(i, j) = texelFetch(NISLuma, sp, 0).r;
        }
    }

    // Directional edge weights from the 2x2 edge map straddling the sample point.
    vec4 e00 = GetEdgeMap(1, 1);
    vec4 e01 = GetEdgeMap(1, 2);
    vec4 e10 = GetEdgeMap(2, 1);
    vec4 e11 = GetEdgeMap(2, 2);
    vec4 w = GetInterpEdgeMap(e00, e01, e10, e11, fx, fy);

    float baseWeight = 1.0 - w.x - w.y - w.z - w.w;
    float opY  = FilterNormal(fx_int, fy_int) * baseWeight;
    opY       += AddDirFilters(fx, fy, fx_int, fy_int, w);

    // Bilinear chroma upscale from the original input + luma-delta reconstruction.
    vec2 coord = vec2((srcX + 0.5) * srcSize.z, (srcY + 0.5) * srcSize.w);
    vec3 op = textureLod(OrigTexture, coord, 0.0).rgb;
    float y = getY(op);
    float corr = opY - y;
    op += vec3(corr);

    FragColor = vec4(clamp(op, 0.0, 1.0), 1.0);
}

#endif
