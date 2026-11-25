#pragma once

#include <string>

/**
 * Upscaler quality bench, run by `--upscaler-bench <spec>` after the GL
 * context is up (the app exits when it finishes). Test tooling, like
 * `--framegen-bench`: it never runs in normal use.
 *
 * Renders the resolution-independent "edges" test pattern (#251) at the
 * target size as the ground truth, builds low-resolution inputs from it,
 * runs each built-in upscaler (UpscalerStage, the same code path as the
 * pipeline) and NEAREST / bilinear baselines up to the target, and prints
 * one "UPSCALE ..." line per (input, method):
 *   psnr  = PSNR against the truth (RGB, dB; higher is better);
 *   ssim  = mean SSIM of 8x8 luma windows (1 = identical);
 *   edge  = Sobel energy of the output / of the truth (1 = as sharp as the
 *           truth, < 1 blurry, > 1 ringing / oversharpened);
 *   ms    = GPU time of one upscale (median of 10, glFinish).
 *
 * Inputs (low = in size):
 *   native  the pattern rendered directly at the low size;
 *   noisy   native + gaussian noise (sigma 4 levels) and a light blur: an
 *           analog capture;
 *   nearest the pattern at the capture size (cap=WxH), decimated to the
 *           low size with NEAREST (what the pipeline does when the logical
 *           capture size is below what the device delivers);
 *   area    the same, decimated with an area average instead.
 *
 * spec: comma-separated key=value
 *   in=WxH    low size (default 640x480)
 *   out=WxH   target (default 1920x1440)
 *   cap=WxH   capture size for nearest / area (default 1280x720)
 *   dump=<dir> also write every image as PPM
 */
int runUpscalerBench(const std::string &spec);
