#include "PixelFormatConverter.h"

#include <algorithm>

namespace rc
{
namespace pixfmt
{
void yuv601ToRgb(int y, int u, int v, uint8_t *out)
{
    int C = y - 16, D = u - 128, E = v - 128;
    int R = (298 * C + 409 * E + 128) >> 8;
    int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
    int B = (298 * C + 516 * D + 128) >> 8;
    out[0] = static_cast<uint8_t>(std::max(0, std::min(255, R)));
    out[1] = static_cast<uint8_t>(std::max(0, std::min(255, G)));
    out[2] = static_cast<uint8_t>(std::max(0, std::min(255, B)));
}

void yuy2ToRgb24(const uint8_t *src, size_t srcSize, uint8_t *dst, uint32_t width, uint32_t height)
{
    if (!src || !dst || width == 0 || height == 0)
        return;
    // YUY2: Y0 U0 Y1 V0 ... (2 pixels per 4 bytes). Bounds-check per macro-pixel.
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; x += 2)
        {
            size_t i = (static_cast<size_t>(y) * width + x) * 2;
            if (i + 3 >= srcSize)
                break;
            int Y0 = src[i], U = src[i + 1], Y1 = src[i + 2], V = src[i + 3];
            yuv601ToRgb(Y0, U, V, dst + (static_cast<size_t>(y) * width + x) * 3);
            if (x + 1 < width)
                yuv601ToRgb(Y1, U, V, dst + (static_cast<size_t>(y) * width + x + 1) * 3);
        }
    }
}

void uyvyToRgb24(const uint8_t *src, size_t srcSize, uint8_t *dst, uint32_t width, uint32_t height)
{
    if (!src || !dst || width == 0 || height == 0)
        return;
    if (static_cast<size_t>(width) * height * 2 > srcSize)
        return; // not enough data — leave as-is
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; x += 2)
        {
            const uint8_t *p = src + (static_cast<size_t>(y) * width + x) * 2;
            int U = p[0], Y0 = p[1], V = p[2], Y1 = p[3];
            yuv601ToRgb(Y0, U, V, dst + (static_cast<size_t>(y) * width + x) * 3);
            if (x + 1 < width)
                yuv601ToRgb(Y1, U, V, dst + (static_cast<size_t>(y) * width + x + 1) * 3);
        }
    }
}

void nv12ToRgb24(const uint8_t *src, size_t srcSize, uint8_t *dst, uint32_t width, uint32_t height)
{
    if (!src || !dst || width == 0 || height == 0)
        return;
    const size_t ySize = static_cast<size_t>(width) * height;
    if (ySize + ySize / 2 > srcSize)
        return; // not enough data — leave as-is
    const uint8_t *yPlane = src;
    const uint8_t *uvPlane = src + ySize;
    for (uint32_t y = 0; y < height; ++y)
    {
        const size_t uvRow = static_cast<size_t>(y / 2) * width;
        for (uint32_t x = 0; x < width; ++x)
        {
            int Y = yPlane[static_cast<size_t>(y) * width + x];
            const uint8_t *uv = uvPlane + uvRow + (x & ~1u);
            yuv601ToRgb(Y, uv[0], uv[1], dst + (static_cast<size_t>(y) * width + x) * 3);
        }
    }
}

void rgb32ToRgb24(const uint8_t *src, size_t srcSize, uint8_t *dst, uint32_t width, uint32_t height)
{
    if (!src || !dst || width == 0 || height == 0)
        return;
    const size_t px = static_cast<size_t>(width) * height;
    if (px * 4 > srcSize)
        return; // not enough data — leave as-is
    for (size_t i = 0; i < px; ++i)
    {
        dst[i * 3 + 0] = src[i * 4 + 0];
        dst[i * 3 + 1] = src[i * 4 + 1];
        dst[i * 3 + 2] = src[i * 4 + 2];
    }
}
} // namespace pixfmt
} // namespace rc
