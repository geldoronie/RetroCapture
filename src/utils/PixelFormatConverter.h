#pragma once

#include <cstddef>
#include <cstdint>

// Pure CPU pixel-format conversions to packed RGB24 (#150).
//
// Extracted from the DirectShow frame grabber (#135) so the BT.601 colour
// math lives in one testable place instead of inline in a capture backend.
// These are dependency-free (no FFmpeg/GL) — usable anywhere a backend has a
// raw buffer and no swscale handy (the custom DirectShow filter path). The
// main app's hot paths (FrameProcessor, MediaEncoder) keep using libswscale,
// which is SIMD-optimized; this is the fallback math, not a replacement.
//
// All converters write tightly-packed RGB24 (3 bytes/pixel, top-to-bottom)
// and bounds-check the source: if the input buffer is too small they leave
// the destination untouched rather than over-reading.
// Nested-namespace form (rc::pixfmt) is spelled out for the GCC 5.5 MXE
// toolchain, which predates C++17's `namespace rc::pixfmt {}` shorthand.
namespace rc
{
namespace pixfmt
{
// BT.601 limited-range YUV → RGB for a single pixel; writes 3 bytes to `out`.
void yuv601ToRgb(int y, int u, int v, uint8_t *out);

// YUY2 / YUYV: Y0 U0 Y1 V0 (2 pixels per 4 bytes).
void yuy2ToRgb24(const uint8_t *src, size_t srcSize, uint8_t *dst, uint32_t width, uint32_t height);

// UYVY: U0 Y0 V0 Y1 (2 pixels per 4 bytes) — byte order swapped vs YUY2.
void uyvyToRgb24(const uint8_t *src, size_t srcSize, uint8_t *dst, uint32_t width, uint32_t height);

// NV12: full-res Y plane (W*H) then interleaved U/V plane (W*H/2), 2x2 subsampled.
void nv12ToRgb24(const uint8_t *src, size_t srcSize, uint8_t *dst, uint32_t width, uint32_t height);

// RGB32 (BGRX/BGRA, 4 bpp) → RGB24: drop the 4th byte, keep byte order.
void rgb32ToRgb24(const uint8_t *src, size_t srcSize, uint8_t *dst, uint32_t width, uint32_t height);
} // namespace pixfmt
} // namespace rc
