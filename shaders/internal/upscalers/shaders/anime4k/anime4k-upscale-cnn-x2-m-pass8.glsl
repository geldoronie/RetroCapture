/*
    MIT License

    Copyright (c) 2019-2021 bloc97
    All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

/*
    Anime4K-v3.2-Upscale-CNN-x2-(M)-Depth-to-Space

    Source: https://github.com/bloc97/Anime4K/blob/master/glsl/Upscale/Anime4K_Upscale_CNN_x2_M.glsl
    (Anime4K v4 / v3.2 CNN weights, unchanged). Converted from the mpv
    user-shader hook format to a libretro GLSL pass by RetroCapture:
    MAIN -> OrigTexture (original input), saved textures -> preset aliases,
    <tex>_pos / _size / _pt / _tex / _texOff -> the equivalent macros below.
    Pass 8 of 9; see ../../anime4k-*.glslp for the pass layout.
    Requires GLSL 1.30+ / GLSL ES 3.00.
*/

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

COMPAT_VARYING vec4 TEX0;

uniform sampler2D OrigTexture;
uniform COMPAT_PRECISION vec4 OriginalSize;
#define MAIN_pos TEX0.xy
#define MAIN_size OriginalSize.xy
#define MAIN_pt OriginalSize.zw
#define MAIN_tex(pos) texture(OrigTexture, pos)
#define MAIN_texOff(off) texture(OrigTexture, TEX0.xy + OriginalSize.zw * (off))
uniform sampler2D conv2d_last_tf;
uniform COMPAT_PRECISION vec4 conv2d_last_tfSize;
#define conv2d_last_tf_pos TEX0.xy
#define conv2d_last_tf_size conv2d_last_tfSize.xy
#define conv2d_last_tf_pt conv2d_last_tfSize.zw
#define conv2d_last_tf_tex(pos) texture(conv2d_last_tf, pos)
#define conv2d_last_tf_texOff(off) texture(conv2d_last_tf, TEX0.xy + conv2d_last_tfSize.zw * (off))

vec4 hook() {
    vec2 f0 = fract(conv2d_last_tf_pos * conv2d_last_tf_size);
    ivec2 i0 = ivec2(f0 * vec2(2.0));
    float c0 = conv2d_last_tf_tex((vec2(0.5) - f0) * conv2d_last_tf_pt + conv2d_last_tf_pos)[i0.y * 2 + i0.x];
    float c1 = c0;
    float c2 = c1;
    float c3 = c2;
    return vec4(c0, c1, c2, c3) + MAIN_tex(MAIN_pos);
}

void main()
{
    FragColor = hook();
}

#endif
