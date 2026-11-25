#include "SourceCrop.h"

#include <algorithm>
#include <cstring>

namespace
{
// std::clamp is C++17; the old MinGW of the Windows build lacks it.
float clampf(float v, float lo, float hi)
{
    return std::max(lo, std::min(hi, v));
}

bool isHorizontal(int side)
{
    return side == SourceCrop::Left || side == SourceCrop::Right;
}
} // namespace

bool SourceCrop::isZero() const
{
    for (float v : value)
    {
        if (v > 0.0f)
            return false;
    }
    return true;
}

void SourceCrop::setSide(int side, float v)
{
    if (side < 0 || side > 3)
        return;
    if (v < 0.0f)
        v = 0.0f;
    if (link == LinkAll)
    {
        for (float &s : value)
            s = v;
        return;
    }
    value[side] = v;
    if (link == LinkPairs)
    {
        // Opposite side: Top<->Bottom, Left<->Right.
        value[side ^ 1] = v;
    }
}

void SourceCrop::setLink(int newLink)
{
    if (newLink < LinkNone || newLink > LinkAll)
        newLink = LinkNone;
    link = newLink;
    if (link == LinkPairs)
    {
        value[Bottom] = value[Top];
        value[Right] = value[Left];
    }
    else if (link == LinkAll)
    {
        const float v = value[Top];
        for (float &s : value)
            s = v;
    }
}

void SourceCrop::setUnit(int newUnit, uint32_t srcW, uint32_t srcH)
{
    if (newUnit != Percent && newUnit != Pixels)
        return;
    if (newUnit == unit)
        return;
    if (srcW > 0 && srcH > 0)
    {
        for (int side = 0; side < 4; ++side)
        {
            const float dim = static_cast<float>(isHorizontal(side) ? srcW : srcH);
            if (newUnit == Pixels)
                value[side] = static_cast<float>(static_cast<int>(value[side] * dim / 100.0f + 0.5f));
            else
                value[side] = value[side] * 100.0f / dim;
        }
    }
    unit = newUnit;
}

float SourceCrop::sidePercent(int side, uint32_t srcW, uint32_t srcH) const
{
    if (side < 0 || side > 3)
        return 0.0f;
    if (unit == Percent)
        return value[side];
    const uint32_t dim = isHorizontal(side) ? srcW : srcH;
    return dim > 0 ? value[side] * 100.0f / static_cast<float>(dim) : 0.0f;
}

void SourceCrop::fractions(uint32_t srcW, uint32_t srcH,
                           float &left, float &right, float &top, float &bottom) const
{
    left = right = top = bottom = 0.0f;
    if (srcW == 0 || srcH == 0)
        return;
    float f[4];
    for (int side = 0; side < 4; ++side)
    {
        f[side] = clampf(sidePercent(side, srcW, srcH) / 100.0f, 0.0f, kMaxSideFraction);
        if (f[side] < 0.00001f)
            f[side] = 0.0f;
    }
    // Opposite sides may not eat the whole frame: scale them down together.
    for (int first = 0; first < 4; first += 2)
    {
        const float total = f[first] + f[first + 1];
        if (total > kMaxTotalFraction)
        {
            const float k = kMaxTotalFraction / total;
            f[first] *= k;
            f[first + 1] *= k;
        }
    }
    top = f[Top];
    bottom = f[Bottom];
    left = f[Left];
    right = f[Right];
}

float SourceCrop::maxSideValue(int side, uint32_t srcW, uint32_t srcH) const
{
    if (unit == Percent)
        return kMaxSideFraction * 100.0f;
    const uint32_t dim = isHorizontal(side) ? srcW : srcH;
    if (dim == 0)
        return 512.0f;
    return static_cast<float>(static_cast<int>(static_cast<float>(dim) * kMaxSideFraction));
}

const char *SourceCrop::unitName(int u)
{
    return u == Pixels ? "px" : "percent";
}

const char *SourceCrop::linkName(int l)
{
    switch (l)
    {
    case LinkPairs:
        return "pairs";
    case LinkAll:
        return "all";
    default:
        return "none";
    }
}

int SourceCrop::unitFromName(const char *name, int fallback)
{
    if (!name)
        return fallback;
    if (std::strcmp(name, "px") == 0 || std::strcmp(name, "pixels") == 0)
        return Pixels;
    if (std::strcmp(name, "percent") == 0 || std::strcmp(name, "%") == 0)
        return Percent;
    return fallback;
}

int SourceCrop::linkFromName(const char *name, int fallback)
{
    if (!name)
        return fallback;
    if (std::strcmp(name, "none") == 0)
        return LinkNone;
    if (std::strcmp(name, "pairs") == 0)
        return LinkPairs;
    if (std::strcmp(name, "all") == 0)
        return LinkAll;
    return fallback;
}

bool detectBlackBorders(const uint8_t *rgb, uint32_t width, uint32_t height,
                        size_t rowStride, int threshold, BlackBorders &out,
                        float minDarkFraction)
{
    out = BlackBorders{};
    if (!rgb || width == 0 || height == 0)
        return false;

    auto dark = [&](uint32_t x, uint32_t y) {
        const uint8_t *p = rgb + static_cast<size_t>(y) * rowStride + static_cast<size_t>(x) * 3;
        const int m = std::max(p[0], std::max(p[1], p[2]));
        return m <= threshold;
    };
    // Sample up to ~512 pixels per line: plenty for a border decision and
    // keeps a 4K frame cheap.
    const uint32_t stepX = std::max<uint32_t>(1, width / 512);
    const uint32_t stepY = std::max<uint32_t>(1, height / 512);
    auto rowIsBorder = [&](uint32_t y) {
        uint32_t n = 0, d = 0;
        for (uint32_t x = 0; x < width; x += stepX, ++n)
            d += dark(x, y) ? 1u : 0u;
        return n > 0 && static_cast<float>(d) >= minDarkFraction * static_cast<float>(n);
    };
    auto colIsBorder = [&](uint32_t x, uint32_t y0, uint32_t y1) {
        uint32_t n = 0, d = 0;
        for (uint32_t y = y0; y < y1; y += stepY, ++n)
            d += dark(x, y) ? 1u : 0u;
        return n > 0 && static_cast<float>(d) >= minDarkFraction * static_cast<float>(n);
    };

    const uint32_t maxRows = static_cast<uint32_t>(static_cast<float>(height) * SourceCrop::kMaxSideFraction);
    const uint32_t maxCols = static_cast<uint32_t>(static_cast<float>(width) * SourceCrop::kMaxSideFraction);

    uint32_t top = 0;
    while (top < height && rowIsBorder(top))
        ++top;
    if (top >= height)
        return false; // whole frame dark: no picture
    uint32_t bottom = 0;
    while (bottom < height - top && rowIsBorder(height - 1 - bottom))
        ++bottom;

    // Columns are judged on the rows that survive the vertical crop, so a
    // letterbox doesn't make every column look mostly dark.
    const uint32_t y0 = top;
    const uint32_t y1 = height - bottom;
    uint32_t left = 0;
    while (left < width && colIsBorder(left, y0, y1))
        ++left;
    if (left >= width)
        return false;
    uint32_t right = 0;
    while (right < width - left && colIsBorder(width - 1 - right, y0, y1))
        ++right;

    out.top = std::min(top, maxRows);
    out.bottom = std::min(bottom, maxRows);
    out.left = std::min(left, maxCols);
    out.right = std::min(right, maxCols);
    return true;
}
