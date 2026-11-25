#pragma once

#include <cstdint>
#include <cstddef>

// #225 — independent 4-side source crop.
//
// Plain value type shared by the UI (sliders, config, presets, web API)
// and the render pipeline (source pre-pass). Values are stored in the
// user's unit (percent of the frame or source pixels); fractions() turns
// them into the per-side fractions the pre-pass needs for a given frame.
struct SourceCrop
{
    enum Side
    {
        Top = 0,
        Bottom = 1,
        Left = 2,
        Right = 3
    };
    enum Unit
    {
        Percent = 0,
        Pixels = 1
    };
    enum Link
    {
        LinkNone = 0,  // every side independent
        LinkPairs = 1, // left = right, top = bottom
        LinkAll = 2    // all four sides equal
    };

    // Hard cap per side, as a fraction of the frame, and on the total of
    // two opposite sides — something is always left to show.
    static constexpr float kMaxSideFraction = 0.45f;
    static constexpr float kMaxTotalFraction = 0.90f;

    float value[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // indexed by Side
    int unit = Percent;
    int link = LinkNone;

    bool isZero() const;

    // Set one side, mirroring it onto the linked sides.
    void setSide(int side, float v);
    // Change the link mode; linked sides take the value of the side that
    // leads them (left for left/right, top for top/bottom; top for all).
    void setLink(int newLink);
    // Change the unit, converting the stored values so the crop stays the
    // same on a srcW x srcH frame. Without known dims the values are kept.
    void setUnit(int newUnit, uint32_t srcW, uint32_t srcH);

    // A side as a percentage of the frame (0 when dims are unknown and the
    // unit is pixels).
    float sidePercent(int side, uint32_t srcW, uint32_t srcH) const;

    // Per-side fractions for a srcW x srcH frame, clamped to the caps.
    void fractions(uint32_t srcW, uint32_t srcH,
                   float &left, float &right, float &top, float &bottom) const;

    // Largest slider value for a side in the current unit.
    float maxSideValue(int side, uint32_t srcW, uint32_t srcH) const;

    // String forms used by the config file and the web API.
    static const char *unitName(int unit);
    static const char *linkName(int link);
    static int unitFromName(const char *name, int fallback);
    static int linkFromName(const char *name, int fallback);
};

// #225 — black-border detection for auto-crop. Scans an RGB24 frame
// (row 0 = top of the picture) from each edge inward and reports how many
// rows/columns are border: a line counts as border when at least
// `minDarkFraction` of its pixels have max(R,G,B) <= `threshold`.
// Each side is capped at SourceCrop::kMaxSideFraction of the frame.
// Returns false (and zeroes the result) when the whole frame is dark —
// there is no picture to find the edges of.
struct BlackBorders
{
    uint32_t top = 0, bottom = 0, left = 0, right = 0;
};
bool detectBlackBorders(const uint8_t *rgb, uint32_t width, uint32_t height,
                        size_t rowStride, int threshold, BlackBorders &out,
                        float minDarkFraction = 0.97f);
