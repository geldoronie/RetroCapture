#pragma once

#include <string>

/**
 * Frame-generation bench (#247), run by `--framegen-bench <spec>` after the
 * GL context is up (the app exits when it finishes). Test tooling, like
 * `--source test`: it never runs in normal use.
 *
 * Takes a sequence of real frames, keeps every `step`-th one, regenerates
 * the frames in between with each method and writes them as raw RGB24 next
 * to the real ones, so tools/framegen-quality-test.sh can score them
 * (PSNR / SSIM with ffmpeg). Also times every GPU stage with glFinish().
 *
 * spec: comma-separated key=value
 *   in=<raw rgb24 file>|motion   input (motion = the #247 test pattern)
 *   size=WxH                     frame size (required for a raw file)
 *   frames=N                     frames to read / render (default 61)
 *   fps=F                        motion pattern frame rate (default 60)
 *   step=K                       keep 1 of K frames (default 2 = drop every 2nd)
 *   out=<dir>                    output directory (default .)
 * Writes out/{ref,repeat,blend,motion,motion_nopick}.rgb and prints
 * "BENCH ..." lines with the timings.
 */
int runFrameGenBench(const std::string &spec);
