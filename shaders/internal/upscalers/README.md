# Built-in upscalers (internal)

These are the passes behind the **Image window > Upscaler** stage (#251).
They are not user shader presets: they don't appear in the Shader tab, and
RetroCapture runs them in a dedicated shader engine, independent of the
user's preset. The user's shader chain runs after them (Position = Before
the shader chain, the default) or before them (After the shader chain).
Every package ships this folder as `shaders/internal/upscalers/`.

| Mode (Image window) | Preset | What it does | Passes |
| --- | --- | --- | --- |
| FSR 1 | `fsr.glslp` | AMD FidelityFX Super Resolution 1.0: EASU edge-adaptive upscale to the target, then RCAS sharpening at the target size. General purpose. | 2 |
| FSR 1 (EASU only) | `fsr-easu.glslp` | FSR 1 EASU only, no sharpening. | 1 |
| NVIDIA NIS | `nis.glslp` | NVIDIA Image Scaling v1.0.3: 6-tap directional scaler + adaptive sharpening, straight to the target. | 3 |
| Anime4K S | `anime4k-cnn-x2-s.glslp` | Anime4K v4 Upscale CNN x2 (S): small CNN, 2x reconstruction, then bilinear to the target. 2D / anime / cartoon / pixel art. | 6 |
| Anime4K M | `anime4k-cnn-x2-m.glslp` | Anime4K v4 Upscale CNN x2 (M): larger CNN, sharper lines, about 2x the cost of S. | 10 |
| Denoise + FSR | `denoise+fsr.glslp` | Hyllian's fast bilateral denoiser at source size, then FSR 1 with noise-aware RCAS. For noisy analog captures. | 3 |

The last pass of every preset renders at the engine's **viewport**, which
RetroCapture sets to the upscale target (Auto = window / output resolution,
2x / 3x / 4x the source, or a custom size).

## Parameters

The Image window's **Sharpness** slider (0..1) drives:

- `FSR_SHARPENING` = 2 x (1 - sharpness): RCAS strength in stops, 0 = sharpest.
- `NIS_SHARPNESS` = sharpness.

`FSR_RCAS_DENOISE` (noise-aware RCAS) is on in `denoise+fsr` only, and
`SIGMA_R` (bilateral strength) keeps its preset value.

## Notes

- **Spatial only.** DLSS, XeSS and FSR 2/3 are temporal: they need engine
  motion vectors and depth that a captured video does not have.
- **Gamma space input.** FSR 1 and NIS (SDR path) expect perceptual,
  sRGB-encoded input, which is what capture frames are.
- **Anime4K** is a fixed 2x network. It looks best when the target is about
  2x the source (e.g. 540p/720p to 1080p/1440p).
- Requires GLSL 1.30+ / GLSL ES 3.00 (`texelFetch`), i.e. any OpenGL 3.x
  desktop context. NIS and Anime4K also need float framebuffers.

## Sources and licenses

All MIT; see `LICENSE.txt`. Each `.glsl` keeps its upstream notice.

- FSR 1: <https://github.com/GPUOpen-Effects/FidelityFX-FSR>, GLSL fragment
  formulation based on libretro glsl-shaders `fsr/`.
- NIS: <https://github.com/NVIDIAGameWorks/NVIDIAImageScaling> (coefficient
  tables generated from `NIS_Config.h`), fragment formulation based on the
  libretro slang-shaders `edge-smoothing/nis/` port.
- Anime4K: <https://github.com/bloc97/Anime4K>, `glsl/Upscale/Anime4K_Upscale_CNN_x2_{S,M}.glsl`,
  converted from mpv hooks to libretro passes with the weights unchanged.
- `shaders/denoise/fast-bilateral.glsl` and `shaders/stock.glsl`: unchanged
  copies of the libretro shaders of the same name in `shaders/shaders_glsl/`.
