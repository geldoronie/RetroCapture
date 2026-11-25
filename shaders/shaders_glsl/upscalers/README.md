# Modern spatial upscalers

Built-in presets for the Shader tab. Every preset renders its final pass at
the **viewport** size, so the image is really upscaled to the output (window /
stream) resolution instead of being upscaled at source size and stretched
afterwards.

| Preset | What it does | Passes |
| --- | --- | --- |
| `fsr.glslp` | AMD FidelityFX Super Resolution 1.0: EASU edge-adaptive upscale to the viewport, then RCAS sharpening at output size. General purpose. | 2 |
| `fsr-easu.glslp` | FSR 1 EASU only (no sharpening). | 1 |
| `denoise+fsr.glslp` | Hyllian's fast bilateral denoiser (`../denoisers`) at source size, then FSR 1 with noise-aware RCAS. For noisy analog captures. | 3 |
| `nis.glslp` | NVIDIA Image Scaling v1.0.3: 6-tap directional scaler + adaptive sharpening, straight to the viewport. | 3 |
| `anime4k-cnn-x2-s.glslp` | Anime4K v4 Upscale CNN x2 (S): small CNN, 2x reconstruction, then bilinear to the viewport. 2D / anime / cartoon / pixel art. | 6 |
| `anime4k-cnn-x2-m.glslp` | Anime4K v4 Upscale CNN x2 (M): larger CNN, sharper lines, about 2x the cost of S. | 10 |

## Parameters

- `FSR_SHARPENING`: RCAS strength in stops, 0 = sharpest; each +1 halves it.
- `FSR_RCAS_DENOISE`: 1 enables RCAS's noise-aware lobe attenuation.
- `NIS_SHARPNESS`: 0..1, NVIDIA's sharpness slider (SDR path).
- `SIGMA_R`: bilateral blur strength in `denoise+fsr`.

## Notes

- **Spatial only.** DLSS, XeSS and FSR 2/3 are temporal: they need engine
  motion vectors and depth that a captured video does not have.
- **Gamma space input.** FSR 1 and NIS (SDR path) expect perceptual,
  sRGB-encoded input, which is what capture frames are. Do not put them after
  a pass that writes linear light.
- **Put them last.** These presets are meant to be the final upscale. To
  combine them with a CRT or NTSC chain, run that chain at source size first;
  a CRT mask upscaled afterwards loses its structure.
- **Anime4K** is a fixed 2x network. It looks best when the viewport is about
  2x the source (e.g. 540p/720p to 1080p/1440p). The mpv original skips itself
  when the output is under 1.2x the source; a preset cannot do that, so choose
  another preset for 1:1 output.
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
