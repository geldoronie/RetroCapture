# RetroCapture

> Real-time video capture for Linux and Windows that applies **RetroArch GLSL
> shaders** (CRT, NTSC, xBR, handheld, …) to live feeds from capture cards.
> Includes HTTP MPEG-TS streaming, local recording, and a full web portal
> for remote control.

![Version](https://img.shields.io/badge/version-0.7.0--alpha-orange)
![License](https://img.shields.io/badge/license-MIT-blue)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows%20%7C%20Raspberry%20Pi-lightgrey)
![Status](https://img.shields.io/badge/status-alpha-yellow)

<!-- SCREENSHOT NEEDED: hero shot. Recommended: native UI fullscreen with a
     well-known CRT preset applied to a recognizable retro game (Sonic / Mario /
     Zelda), or the web portal Home page with the live stream playing.
     Target ~1600x900. Save as docs/screenshots/hero.png -->
![Hero](docs/screenshots/hero.png)

**[⬇ Download 0.7.0-alpha](https://github.com/geldoronie/RetroCapture/releases/latest)** ·
[Documentation](#documentation) ·
[Issues](https://github.com/geldoronie/RetroCapture/issues) ·
[Changelog](CHANGELOG.md)

---

## What it does

RetroCapture turns a generic capture card into a "retro-aware" capture rig:

- 🎮 **Real-time capture** from V4L2 (Linux) or DirectShow (Windows) devices.
- 🌈 **RetroArch GLSL shaders applied live** — single-pass and multi-pass
  presets, including `PassFeedback` and the `OriginalHistory` alias chain.
- 📡 **HTTP MPEG-TS streaming** with H.264 / H.265 (VP8 / VP9 experimental)
  and PulseAudio / WASAPI system-audio capture.
- ⏺️ **Local recording** to MP4 / MKV / AVI with AAC / MP3 / Opus audio,
  configurable resolution, FPS and bitrates.
- 💾 **Named profiles** for both streaming and recording — save / load / delete
  full configs by name from either the native UI or the web portal.
- 🌐 **Web portal**: live stream player on Home, recordings browser with
  thumbnails, full configuration UI, installable as a PWA.
- 🎛️ **Hardware controls** exposed directly (V4L2 / DirectShow brightness,
  contrast, saturation, hue, gain, exposure, gamma, white balance, …).
- 🥧 **Cross-platform**: Linux x86_64, Windows x86_64, Raspberry Pi 4/5
  (ARM64), Raspberry Pi 2/3/Zero (ARM32v7).

---

## At a glance

### Shader comparison

RetroCapture applies RetroArch presets in real time. Same capture, different
shader:

| Without shader | CRT Mattias |
| -------------- | ----------- |
| ![Without Shader - Mattias](docs/sonic-no-shaders-mattias.png) | ![With Shader - Mattias](docs/sonic-with-shaders-mattias.png) |

| Without shader | NTSC |
| -------------- | ---- |
| ![Without Shader - NTSC](docs/sonic-no-shaders-ntsc.png) | ![With Shader - NTSC](docs/sonic-with-shaders-ntsc.png) |

### Native UI

<!-- SCREENSHOT NEEDED: native ImGui UI with the Shaders tab open and a CRT
     preset selected, showing the parameter sliders on the right. Capture at
     ~1280x720. Save as docs/screenshots/native-ui-shaders.png -->
![Native UI — Shaders tab](docs/screenshots/native-ui-shaders.png)

### Web portal — Home (live player)

<!-- SCREENSHOT NEEDED: web portal Home page, browser window at ~1280x800.
     Show the embedded MPEG-TS player playing real content, the status panel
     (FPS, bitrate, clients), and the top navigation. Save as
     docs/screenshots/portal-home.png -->
![Web portal — Home](docs/screenshots/portal-home.png)

### Web portal — Recordings browser

<!-- SCREENSHOT NEEDED: web portal Recordings page with a few recordings
     listed (thumbnails visible, durations, sizes). Save as
     docs/screenshots/portal-recordings.png -->
![Web portal — Recordings](docs/screenshots/portal-recordings.png)

### Web portal — Configuration

<!-- SCREENSHOT NEEDED: web portal Configuration page on the Stream or
     Recording section, showing the profile selector + codec / bitrate
     controls. Save as docs/screenshots/portal-config.png -->
![Web portal — Configuration](docs/screenshots/portal-config.png)

### Mobile (PWA)

<!-- SCREENSHOT NEEDED: phone-sized capture (e.g. 390x844) of the portal
     installed as a PWA. Save as docs/screenshots/portal-mobile.png -->
![Web portal on mobile](docs/screenshots/portal-mobile.png)

### Profiles in action

<!-- SCREENSHOT NEEDED: profile management modal/panel — either the recording
     profile selector with a few saved profiles or the streaming one. Save as
     docs/screenshots/profiles.png -->
![Named profiles](docs/screenshots/profiles.png)

---

## Download

Pre-built binaries for **0.7.0-alpha** are attached to the
[latest GitHub release](https://github.com/geldoronie/RetroCapture/releases/latest):

| Platform | Artifact |
| --- | --- |
| Linux x86_64 | `RetroCapture-0.7.0-alpha-linux-x86_64.AppImage` |
| Linux ARM64 (Raspberry Pi 4 / 5) | `RetroCapture-0.7.0-alpha-linux-arm64v8.tar.gz` |
| Linux ARM32v7 (Raspberry Pi 2 / 3 / Zero 2) | `RetroCapture-0.7.0-alpha-linux-arm32v7.tar.gz` |
| Windows x86_64 | `RetroCapture-0.7.0-alpha-windows-x86_64-Setup.exe` |

A `SHA256SUMS` file is published alongside the binaries.

> The Linux x86_64 build targets a portable instruction-set baseline
> (no AVX/AVX2) so it runs on a wide range of CPUs including older
> Sandy/Ivy Bridge, low-power and virtualized systems. See
> [`docs/CPU_COMPATIBILITY.md`](docs/CPU_COMPATIBILITY.md) for details.

To build from source instead, see [Building](#building) below.

---

## Status

RetroCapture is **alpha** and actively developed. Most things work; some
edges are still being polished.

| Area | State |
| --- | --- |
| Live capture (V4L2 / DirectShow) | ✅ Stable |
| Shader pipeline (single-pass, multi-pass, PassFeedback, OriginalHistory) | ✅ Stable |
| H.264 streaming | ✅ Stable |
| H.265 streaming | ✅ Stable |
| VP8 / VP9 streaming | ⚠️ Functional but may show as "Data: bin_data" in some players |
| Local recording (MP4 / MKV / AVI) | ✅ Stable as of 0.6.0 (PTS fix landed) |
| Web portal | ✅ Stable (full overhaul in 0.6.0) |
| Profiles (streaming + recording) | ✅ New in 0.6.0 |
| Raspberry Pi (ARM64 / ARM32) | ✅ Headless via SDL2 + DirectFB |

**Shader compatibility note:** most CRT, NTSC, upscale (xBR, Super-xBR) and
handheld presets work cleanly. A handful of complex multi-pass presets are
still being shaken out — bug reports very welcome.

For the full per-version history see [`CHANGELOG.md`](CHANGELOG.md).

---

## Quick start

### Linux (AppImage)

```bash
# Download from Releases, then:
chmod +x RetroCapture-0.7.0-alpha-linux-x86_64.AppImage
./RetroCapture-0.7.0-alpha-linux-x86_64.AppImage --source v4l2 --v4l2-device /dev/video0
```

### Windows

Run the installer (`RetroCapture-0.7.0-alpha-windows-x86_64-Setup.exe`),
then launch RetroCapture from the Start menu. DirectShow is the default
capture source on Windows.

### Raspberry Pi

```bash
tar -xzf RetroCapture-0.7.0-alpha-linux-arm64v8.tar.gz
cd RetroCapture-0.7.0-alpha-linux-arm64v8
./retrocapture --source v4l2 --v4l2-device /dev/video0
```

For Pi 2 / 3 / Zero 2, use the `arm32v7` tarball instead.

### Useful flags

```bash
# Apply a shader preset
./retrocapture --preset shaders/shaders_glsl/crt/crt-guest-dr-venom.glslp

# Custom resolution and framerate
./retrocapture --width 1920 --height 1080 --fps 60

# Fullscreen on monitor 1
./retrocapture --fullscreen --monitor 1 --maintain-aspect

# No capture device (dummy mode for testing)
./retrocapture --source none

# Custom web portal port + HTTPS
./retrocapture --web-portal-port 9000 \
               --web-portal-https \
               --web-portal-ssl-cert ssl/server.crt \
               --web-portal-ssl-key ssl/server.key
```

Press **F12** to toggle the native UI. Run `./retrocapture --help` for the
full list of command-line flags (source selection, hardware controls,
streaming, web portal, …).

---

## Streaming

Once streaming is enabled (UI Stream tab, web portal, or `--stream-enable`),
the MPEG-TS stream is served at `http://<host>:8080/stream`:

```bash
# ffplay
ffplay http://localhost:8080/stream

# VLC
vlc http://localhost:8080/stream

# mpv
mpv http://localhost:8080/stream
```

Multiple clients can connect simultaneously. Bitrate, codec, resolution and
audio are configured from the UI or via the REST API.

---

## Web portal

The web portal runs on the same port as the stream (default `8080`) and
gives you full remote control of the application from any browser on
your network:

- **Home** — live MPEG-TS player (vendored `mpegts.js` with native fallback)
  plus live status (FPS, bitrate, connected clients).
- **Recordings** — browse, download, rename and delete recordings with
  generated thumbnails.
- **Configuration** — every other setting: source, shader, image processing,
  stream, recording, hardware controls, profiles.
- **Offline / PWA** — Bootstrap and Bootstrap Icons are fully vendored and
  the portal works offline via a service worker; can be installed as a PWA
  on mobile and desktop.

For PWA installation over a local-network IP, HTTPS is required.
See [`ssl/README.md`](ssl/README.md) for certificate generation.

---

## Building

> Production builds use the Docker scripts, which default to a portable
> instruction-set baseline. Native CMake builds default to `-march=native`.
> See [`docs/CPU_COMPATIBILITY.md`](docs/CPU_COMPATIBILITY.md).

### One-shot — build every release artifact

```bash
./tools/package-release.sh
```

This runs every platform build in sequence (AppImage x86_64, Linux ARM64,
Linux ARM32v7, Windows installer) and emits artifacts under `dist/` plus a
`SHA256SUMS` file. Use `--only-x86`, `--only-arm`, `--only-win` etc. to
build a subset.

### Per-platform scripts

```bash
# Linux x86_64 AppImage (Docker — portable baseline)
./tools/build-linux-appimage-x86_64.sh

# Linux ARM64 / ARM32 cross-compile (Docker + qemu)
./tools/build-linux-arm64v8-docker.sh
./tools/build-linux-arm32v7-docker.sh

# Windows installer (NSIS via MXE Docker)
./tools/build-windows-installer.sh

# Native Linux build (uses your local toolchain)
./build.sh
```

All Docker scripts auto-build their image on first run.

### Native build on Raspberry Pi

```bash
./tools/install-deps-raspberry-pi.sh
./tools/build-on-raspberry-pi.sh
```

### Dependencies

**Linux**: V4L2, OpenGL 3.3+, GLFW 3.x, libv4l2, libpng, X11, CMake 3.10+,
C++17 compiler.

**Windows**: DirectShow-capable device, OpenGL 3.3+, CMake 3.10+, MinGW-w64
or MSVC.

**Both**: FFmpeg 6.x (libavcodec / libavformat / libavutil / libswscale /
libswresample) with libx264 (mandatory) and libx265 / libvpx (optional);
nlohmann/json (auto-fetched by CMake); OpenSSL (optional, for HTTPS).

---

## Documentation

- [`CHANGELOG.md`](CHANGELOG.md) — full version history.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — system architecture,
  components, data flow, threading model.
- [`docs/PATHS.md`](docs/PATHS.md) — on-disk layout (assets / config /
  data / cache / recordings) and the env-var overrides for each role.
- [`docs/REMOTE_STREAM_PROTOCOL.md`](docs/REMOTE_STREAM_PROTOCOL.md) —
  wire format of `/stream`, `/raw`, `/meta` and the Remote source
  client.
- [`docs/DIRECTORY_PROTOCOL.md`](docs/DIRECTORY_PROTOCOL.md) — opt-in
  public stream directory protocol.
- [`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md) — commit conventions
  and development process.
- [`docs/CPU_COMPATIBILITY.md`](docs/CPU_COMPATIBILITY.md) —
  instruction-set baselines per architecture, diagnostic workflow.

---

## Contributing

Bug reports, shader-compatibility reports and PRs are very welcome.

Before opening a PR, please read [`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md)
for commit conventions and [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
for an orientation tour of the codebase.

When reporting a shader that misrenders, please include:

- The preset path (e.g. `shaders/shaders_glsl/crt/<name>.glslp`)
- Any error messages from the log
- Expected vs. actual behavior (a screenshot helps a lot)
- GPU + driver version

---

## License

MIT — see [`LICENSE`](LICENSE).

---

## Acknowledgments

- The **RetroArch community** for the extensive shader library and the
  GLSL spec that this project implements against.
- The maintainers of **GLFW**, **Dear ImGui**, **FFmpeg**, **mpegts.js**,
  **Bootstrap** and **nlohmann/json**.

**Note:** RetroCapture is an independent project and is not affiliated with
RetroArch, Libretro, Sega, Nintendo, or any other trademark referenced.
