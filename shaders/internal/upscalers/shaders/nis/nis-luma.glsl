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
    NVIDIA Image Scaling (NIS) v1.0.3 - luma prepass.

    Source: https://github.com/NVIDIAGameWorks/NVIDIAImageScaling
    Pass layout follows the libretro slang-shaders `edge-smoothing/nis` port.

    Computes Rec.709 luma at source resolution once, so the scaler pass reads
    a 6x6 luma support per output pixel with texelFetch instead of recomputing
    it from RGB. Run at scale_type = source with float_framebuffer = true so
    the luma keeps more than 8 bits of precision.
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
#define COMPAT_TEXTURE texture
out vec4 FragColor;
#else
#define COMPAT_VARYING varying
#define FragColor gl_FragColor
#define COMPAT_TEXTURE texture2D
#endif

#ifdef GL_ES
precision highp float;
#endif

uniform sampler2D Texture;
COMPAT_VARYING vec4 TEX0;

void main()
{
    vec3 c = COMPAT_TEXTURE(Texture, TEX0.xy).rgb;
    FragColor = vec4(0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b, 0.0, 0.0, 1.0);
}

#endif
