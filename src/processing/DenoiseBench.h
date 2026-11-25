#pragma once

#include <string>

/**
 * Temporal denoise bench (#268), run by `--denoise-bench <spec>` after the
 * GL context is up (the app exits when it finishes). Test tooling, like
 * `--upscaler-bench`: it never runs in normal use.
 *
 * Renders the "motion" test pattern offline (the exact frames the test
 * source delivers): the first half of the clip moves (60 fps), the second
 * half is still. Every frame gets fresh analog-like noise (the same model as
 * RETROCAPTURE_TEST_NOISE), goes through TemporalDenoiser (the pipeline's
 * code) and is compared with the clean frame. One "DENOISE ..." line per
 * strength:
 *   moving  mean PSNR (dB, RGB) over the moving frames: noise removed AND
 *           no ghosting/smear (both lower it);
 *   still   mean PSNR over the still frames after the history settles;
 *   ms      GPU time of one frame (median, glFinish).
 * strength 0 is the input itself (the noisy baseline).
 *
 * spec: comma-separated key=value
 *   size=WxH        frame size (default 640x480)
 *   frames=N        clip length (default 120: 60 moving + 60 still)
 *   noise=S         sigma in 8-bit levels (default 8; 0 = clean input, to
 *                   check the filter leaves a clean source alone)
 *   strengths=a:b   strengths to run (default 0:0.25:0.5:0.75:1)
 *   dump=<dir>      write the noisy / filtered frame 20 (moving) as PPM
 */
int runDenoiseBench(const std::string &spec);
