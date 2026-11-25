#pragma once

// #227 — Output geometry: integer scaling, display-aspect override and
// rotation / mirror, applied at the final presentation stage (window draw)
// and to the frame handed to the outputs (stream / recording / virtcam).
//
// Header-only on purpose: pure math, no GL calls, shared by the pipeline,
// the UI and the web API.

#include <algorithm>
#include <cmath>
#include <cstdint>

struct OutputGeometry
{
    // Display aspect ratio of the (unrotated) image.
    enum class Aspect : int
    {
        Auto = 0,  // source width / height (previous behaviour)
        R4_3 = 1,
        R8_7 = 2,  // NES/SNES square pixels (256x224)
        R16_9 = 3,
        R10_9 = 4, // Game Boy (160x144)
        R3_2 = 5,  // Game Boy Advance (240x160)
        Custom = 6
    };

    // Window presentation scaling. Outputs have their own fixed frame
    // size, so this only affects the local window.
    enum class IntegerScale : int
    {
        Off = 0,     // fit (or stretch, see maintainAspect)
        Integer = 1, // both axes integer multiples of the source pixel grid
        IntegerFill = 2 // integer on the scanline axis, the other axis
                        // fills the exact target aspect
    };

    IntegerScale integerScale = IntegerScale::Off;
    Aspect aspect = Aspect::Auto;
    float customAspectW = 4.0f;
    float customAspectH = 3.0f;
    int rotation = 0; // degrees clockwise: 0, 90, 180, 270
    bool flipH = false;
    bool flipV = false;

    static int normalizeRotation(int deg)
    {
        int r = ((deg % 360) + 360) % 360;
        // Snap to the nearest quarter turn.
        r = ((r + 45) / 90) * 90;
        return r % 360;
    }

    bool swapsAxes() const
    {
        const int r = normalizeRotation(rotation);
        return r == 90 || r == 270;
    }

    bool hasOrientation() const
    {
        return normalizeRotation(rotation) != 0 || flipH || flipV;
    }

    // True when the outputs need a geometry pass (integer scaling is a
    // window-only concern and never triggers one).
    bool affectsOutputs() const
    {
        return hasOrientation() || aspect != Aspect::Auto;
    }

    // Aspect override set (or integer scaling on): the window always
    // letterboxes, regardless of the "Maintain Aspect Ratio" toggle.
    bool forcesAspect() const
    {
        return aspect != Aspect::Auto || integerScale != IntegerScale::Off;
    }

    // Display aspect (w/h) of the unrotated image.
    float displayAspect(uint32_t srcW, uint32_t srcH) const
    {
        switch (aspect)
        {
        case Aspect::R4_3:  return 4.0f / 3.0f;
        case Aspect::R8_7:  return 8.0f / 7.0f;
        case Aspect::R16_9: return 16.0f / 9.0f;
        case Aspect::R10_9: return 10.0f / 9.0f;
        case Aspect::R3_2:  return 3.0f / 2.0f;
        case Aspect::Custom:
            if (customAspectW > 0.0f && customAspectH > 0.0f)
                return customAspectW / customAspectH;
            break;
        case Aspect::Auto:
        default:
            break;
        }
        if (srcW == 0 || srcH == 0)
            return 4.0f / 3.0f;
        return static_cast<float>(srcW) / static_cast<float>(srcH);
    }

    // Display size (rotated) of the image at 1x vertical scale: height is
    // the source height, width follows the display aspect.
    void displaySize(uint32_t srcW, uint32_t srcH, uint32_t &outW, uint32_t &outH) const
    {
        const float a = displayAspect(srcW, srcH);
        uint32_t h = std::max<uint32_t>(1, srcH);
        uint32_t w = aspect == Aspect::Auto
                         ? std::max<uint32_t>(1, srcW)
                         : std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(h * a)));
        if (swapsAxes())
            std::swap(w, h);
        outW = w;
        outH = h;
    }

    struct Rect
    {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
    };

    // Largest rect of aspect a (w/h) inside W x H, centred.
    static Rect fitRect(float a, int W, int H)
    {
        Rect r;
        if (W <= 0 || H <= 0 || a <= 0.0f)
        {
            r.w = std::max(0, W);
            r.h = std::max(0, H);
            return r;
        }
        const float winA = static_cast<float>(W) / static_cast<float>(H);
        if (a > winA)
        {
            r.w = W;
            r.h = std::max(1, static_cast<int>(std::lround(W / a)));
        }
        else
        {
            r.h = H;
            r.w = std::max(1, static_cast<int>(std::lround(H * a)));
        }
        r.x = (W - r.w) / 2;
        r.y = (H - r.h) / 2;
        return r;
    }

    // Where the image lands in a W x H window. srcW/srcH: size of the
    // texture being presented (post-shader / output resolution).
    // maintainAspect: the legacy toggle; only consulted in Auto aspect
    // without integer scaling (false there = stretch to the window).
    Rect windowRect(uint32_t srcW, uint32_t srcH, int W, int H, bool maintainAspect) const
    {
        Rect full;
        full.w = std::max(0, W);
        full.h = std::max(0, H);
        if (W <= 0 || H <= 0 || srcW == 0 || srcH == 0)
            return full;

        const bool swap = swapsAxes();
        float a = displayAspect(srcW, srcH); // unrotated
        if (integerScale != IntegerScale::Off)
        {
            // Work in the unrotated frame: the scanline axis is the
            // source's vertical axis.
            const int uw = swap ? H : W;
            const int uh = swap ? W : H;
            for (int ky = uh / static_cast<int>(srcH); ky >= 1; --ky)
            {
                const int h = ky * static_cast<int>(srcH);
                int w;
                if (integerScale == IntegerScale::Integer)
                {
                    int kx = static_cast<int>(std::lround(h * a / static_cast<float>(srcW)));
                    // Closest integer to the target aspect; if it does
                    // not fit, drop a vertical step rather than skewing
                    // the aspect.
                    kx = std::max(1, kx);
                    w = kx * static_cast<int>(srcW);
                }
                else
                {
                    w = static_cast<int>(std::lround(h * a));
                }
                if (w <= uw)
                {
                    Rect r;
                    r.w = swap ? h : w;
                    r.h = swap ? w : h;
                    r.x = (W - r.w) / 2;
                    r.y = (H - r.h) / 2;
                    return r;
                }
            }
            // Window smaller than 1x: fall back to fit.
        }

        if (!forcesAspect() && !maintainAspect)
            return full; // stretch (legacy "Maintain Aspect" off)

        if (swap)
            a = 1.0f / a;
        return fitRect(a, W, H);
    }

    // 2x2 texture-coordinate transform (row-major m00 m01 m10 m11) applied
    // around the texture centre: uv' = M * (uv - 0.5) + 0.5. The visual
    // transform is "rotate clockwise, then mirror in the output
    // orientation". yDown: the target's visual space has +y pointing down
    // (an FBO read back top-down for the encoders) instead of the window's
    // y-up, which reverses the rotation direction.
    void uvMatrix(bool yDown, float m[4]) const
    {
        // M = Rccw(theta) * Mirror  (inverse of Mirror * Rcw(theta))
        const int r = normalizeRotation(rotation);
        float c = 1.0f, s = 0.0f;
        if (r == 90)       { c = 0.0f;  s = 1.0f; }
        else if (r == 180) { c = -1.0f; s = 0.0f; }
        else if (r == 270) { c = 0.0f;  s = -1.0f; }
        const float mx = flipH ? -1.0f : 1.0f;
        const float my = flipV ? -1.0f : 1.0f;
        // Rccw = [[c, -s], [s, c]], Mirror = diag(mx, my)
        m[0] = c * mx;
        m[1] = -s * my;
        m[2] = s * mx;
        m[3] = c * my;
        if (yDown)
        {
            // F * M * F with F = diag(1, -1)
            m[1] = -m[1];
            m[2] = -m[2];
        }
    }

    // Frame handed to the outputs. targetW/H: the frame size the outputs
    // expect (encoder size); 0 = unknown, use the display size. The image
    // is fitted (letterboxed) into the canvas so the encoder's plain resize
    // to its fixed size does not distort the rotated / re-aspected image.
    void outputCanvas(uint32_t srcW, uint32_t srcH, uint32_t targetW, uint32_t targetH,
                      uint32_t &canvasW, uint32_t &canvasH, Rect &content) const
    {
        uint32_t dw = 0, dh = 0;
        displaySize(srcW, srcH, dw, dh);
        if (targetW > 0 && targetH > 0)
        {
            canvasW = targetW;
            canvasH = targetH;
            float a = displayAspect(srcW, srcH);
            if (swapsAxes())
                a = 1.0f / a;
            content = fitRect(a, static_cast<int>(canvasW), static_cast<int>(canvasH));
            return;
        }
        // Even dimensions keep 4:2:0 encoders happy.
        canvasW = std::max<uint32_t>(2, dw & ~1u);
        canvasH = std::max<uint32_t>(2, dh & ~1u);
        content.x = 0;
        content.y = 0;
        content.w = static_cast<int>(canvasW);
        content.h = static_cast<int>(canvasH);
    }
};
