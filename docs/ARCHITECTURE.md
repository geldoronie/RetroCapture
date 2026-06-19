# RetroCapture Architecture

This document describes the runtime architecture of RetroCapture as of
**0.7.0-alpha** — the components, how they fit together, and where each
responsibility lives in the source tree. It is intended as the
orientation tour for new contributors and the reference point when
reviewing changes that cut across subsystems.

---

## Big picture

RetroCapture is a single native binary that captures video and audio
from a local device, optionally pushes the video through a GLSL shader
chain, and then routes the result to four downstream consumers:

1. The **on-screen window** (interactive use).
2. A **local recording** to disk (`MP4` / `MKV` / `AVI`).
3. An **HTTP MPEG-TS stream** served on a local port (`/stream`) for
   browser / VLC / ffplay viewers.
4. A **raw, pre-shader feed** on the same port (`/raw`) plus a `/meta`
   side channel, consumed by another RetroCapture running in **Remote
   source** mode (shader-preserving distributed playback, see #47).

The same binary also hosts the **web portal** (configuration UI, live
stream, recordings browser, PWA) and an opt-in **public stream
directory client** that lets other RetroCapture instances discover the
stream over the internet through Cloudflare tunnels.

```
                          ┌────────────────────────┐
                          │       Application      │
                          │   (src/core/, main)    │
                          └───────────┬────────────┘
                                      │ owns + ticks
        ┌───────────────┬─────────────┼─────────────┬────────────┐
        ▼               ▼             ▼             ▼            ▼
┌───────────────┐ ┌───────────┐ ┌───────────┐ ┌───────────┐ ┌──────────┐
│  Capture      │ │ Audio     │ │ Window +  │ │  UI       │ │  OSD     │
│ (V4L2/DS/     │ │ (Pulse/   │ │ Renderer  │ │ (ImGui)   │ │ overlays │
│  Remote)      │ │  WASAPI)  │ │ (OpenGL)  │ │           │ │          │
└─────┬─────────┘ └────┬──────┘ └─────┬─────┘ └─────┬─────┘ └────┬─────┘
      │                │              │             │            │
      ▼                │              │             │            │
┌───────────────┐      │              │             │            │
│ FrameProcessor│      │              │             │            │
│ (YUYV→RGB,    │      │              │             │            │
│  upload tex)  │      │              │             │            │
└─────┬─────────┘      │              │             │            │
      │                │              │             │            │
      ▼                │              │             │            │
┌───────────────┐      │              │             │            │
│ ShaderEngine  │      │              │             │            │
│ (.slangp /    │      │              │             │            │
│  .glslp)      │      │              │             │            │
└─────┬─────────┘      │              │             │            │
      │                │              │             │            │
      ├────────────────┼──────────────┘             │            │
      │                │                            │            │
      ▼                ▼                            │            │
┌──────────────────────────────┐       ┌────────────┴────────────┴───────┐
│ RecordingManager             │       │  WebPortal + APIController       │
│ ├─ MediaSynchronizer (A/V)   │       │  (live stream <iframe>, configs, │
│ ├─ MediaEncoder (FFmpeg)     │       │   recordings, profiles, PWA)     │
│ └─ FileRecorder              │       └──────────────────────────────────┘
└──────────────────────────────┘                       ▲
                                                       │  HTTP
┌─────────────────────────────────────────────┐       │
│ StreamManager → HTTPTSStreamer + IStreamer  │───────┤
│ ├─ MediaSynchronizer (per-endpoint)         │       │
│ ├─ MediaEncoder (post-shader → /stream)     │       │
│ └─ RawMediaEncoder (pre-shader → /raw)      │───────┘
└─────────────────────────────────────────────┘
              │                       │
              ▼                       ▼
        HTTP MPEG-TS            /raw + /meta
        (browsers, VLC,       (RetroCapture in
         ffplay)               Remote source mode)
                                       │
                                       ▼
                             ┌─────────────────────────┐
                             │ VideoCaptureRemote +    │
                             │ RemoteMetaSync          │
                             │ (decodes upstream feed, │
                             │  mirrors host's shader/ │
                             │  preset/params)         │
                             └─────────────────────────┘

                 ┌───────────────────────────────────────────┐
                 │ DirectoryClient / DirectoryBrowser (#49)  │
                 │ CloudflaredManager  (Quick / Named, #53)  │
                 │ → directory.retrocapture.com (HTTPS)      │
                 └───────────────────────────────────────────┘
```

---

## Application lifecycle

`main.cpp` parses CLI flags, then constructs the single `Application`
object (`src/core/Application.{h,cpp}`). The Application owns every
long-lived subsystem as a `std::unique_ptr` and drives them from one
main loop. There is no service container, no DI framework — wiring is
explicit.

The loop, in order each tick:

1. Poll window events (GLFW).
2. **Capture**: pull the latest frame from whichever
   `IVideoCapture` implementation is active (V4L2, DirectShow,
   Remote).
3. **Audio capture**: drain the audio ring buffer maintained by the
   `IAudioCapture` worker.
4. **Process**: `FrameProcessor` converts YUYV → RGB (if needed) and
   uploads to the source texture; `ShaderEngine` applies the active
   `.slangp` / `.glslp` preset to produce the rendered texture.
5. **Render**: the rendered texture is drawn to the window, the OSD
   overlays composite on top, and the ImGui frame is built.
6. **Feed downstream consumers**:
   - `StreamManager::pushFrame` → `MediaSynchronizer` →
     `MediaEncoder` (post-shader, served as `/stream`).
   - `StreamManager::pushRawFrame` → second encoder pair (pre-shader,
     served as `/raw`). Gated on `hasRawClients()` so the second
     encoder idles when no Remote viewer is connected.
   - `RecordingManager::pushFrame` (when recording).
7. **Tick out-of-loop subsystems** that don't ride the per-frame
   timeline: `DirectoryClient` (publish heartbeat),
   `CloudflaredManager` (tunnel supervision), `RemoteMetaSync`
   (in Remote source mode, poll the upstream `/meta`).

Subsystems shut down in LIFO order at scope exit — the
`unique_ptr` destructors handle thread joins and resource release.

---

## Components

Each component below corresponds to a top-level directory under
`src/`. The bullet under each name lists the primary classes and the
files they live in.

### `src/core/` — Orchestration

- **`Application`** — owns every subsystem, drives the main loop,
  centralises config persistence and source-type switching (V4L2 /
  DirectShow / Remote). All cross-subsystem wiring happens here, not
  inside the subsystems themselves.

### `src/capture/` — Video capture sources

All concrete captures implement `IVideoCapture`. The `Frame` struct
exposes a raw pixel buffer + width/height/format/timestamp.
`VideoCaptureFactory` picks the implementation based on the
`SourceType` selected in the UI.

- **`VideoCaptureV4L2`** — Linux V4L2 mmap'd buffers, YUYV native.
- **`VideoCaptureDS`** — Windows DirectShow filter graph (uses the
  helper grabber in `DSFrameGrabber`/`DSPin`), RGB24 native.
- **`VideoCaptureRemote`** — consumes an upstream RetroCapture's
  `/raw` MPEG-TS stream over HTTP/HTTPS. Decodes via FFmpeg's avio
  (TLS transparently handled), runs its own decode thread, exposes a
  bounded queue with PTS-anchored playback timing and pluggable
  interpolation modes.

### `src/audio/` — Audio capture + playback

- **`IAudioCapture`** + `AudioCapturePulse` / `AudioCaptureWASAPI` —
  per-platform system-audio capture; on Linux this creates a
  PulseAudio null-sink + monitor source so applications route through
  us transparently.
- **`IAudioPlayback`** + `AudioPlaybackPulse` / `AudioPlaybackWASAPI` —
  used only in Remote source mode to play back the decoded audio from
  the upstream stream. `getClockUs()` is the A/V master clock the
  video consumer paces against.

### `src/processing/` — Frame-data preparation

- **`FrameProcessor`** — owns the source texture, runs YUYV→RGB via
  `sws_scale` when needed, handles the texture upload to OpenGL, and
  exposes the texture handle for the renderer / shader engine.

### `src/shader/` — Shader pipeline

- **`ShaderEngine`** — loads `.slangp` and `.glslp` presets, manages
  the multi-pass FBO graph (including the `PassFeedback`,
  `OriginalHistory`, `PassOutput` and history-alias chains documented
  in `shaders/`'s README), and exposes `applyShader(inputTex, w, h) →
  outputTex`. The preset describes one or more passes; the engine
  compiles, links and dispatches them.
- **`ShaderPreprocessor`** — slang→GLSL transpilation glue.
- **`ShaderPreset`** — parsed representation of a `.glslp` /
  `.slangp` file.

### `src/renderer/` — OpenGL plumbing

- **`OpenGLRenderer`** — the simple textured-quad renderer used to
  draw the final texture to the window and to do format-conversion
  passes (RGBA→RGB). Carries a `flipY` uniform; callers decide
  whether the source needs a Y-flip on the way to the framebuffer.
- **`PBOManager`** — pixel-pack-buffer manager used during
  `glReadPixels` for streaming / recording capture to avoid stalls.
- **`OpenGLStateTracker`** — minimal GL state cache.
- **`glad_loader`** — function loading.

### `src/output/` — Windowing

- **`WindowManager`** (GLFW) / **`WindowManagerSDL`** — the
  GLFW-based path is the default; the SDL path was a planned
  alternative and is currently unused.

### `src/recording/` — Local recording

- **`RecordingManager`** — orchestrates one recording session.
- **`MediaSynchronizer`** — the overlap-gated A/V sync state machine
  (50 ms tolerance, configurable). Shared with the streaming path so
  both wire formats use identical sync semantics.
- **`MediaEncoder`** / **`MediaMuxer`** — FFmpeg encoder + muxer
  wrapper used by both the recording and streaming paths.
- **`FileRecorder`** — file-write target for the recording path.
- **`RecordingProfileManager`** — named profiles (save / load /
  delete).
- **`RecordingMetadata`** — sidecar JSON written alongside each
  recording (resolution, codec, shader, thumbnail path, …).

### `src/streaming/` — Streaming + remote services

Wide directory; grouped below by responsibility.

**HTTP server + on-the-wire streaming**

- **`HTTPServer`** — small thread-pool HTTP/1.1 server used for the
  whole webby surface (portal, API, `/stream`, `/raw`, `/meta`,
  thumbnails).
- **`HTTPTSStreamer`** — implements `IStreamer`. Owns two encoder
  pipelines (post-shader for `/stream`, pre-shader for `/raw`) plus
  the synchronizers, manages the client lists, and inlines
  HEVC VPS/SPS/PPS / AAC ADTS on mid-join so newly-connected viewers
  can decode immediately.
- **`StreamManager`** — thin coordinator: picks the streamer
  implementation, owns its lifetime, fans `pushFrame`/`pushAudio`
  out.
- **`StreamingProfileManager`** — named streaming profiles.

**Web portal**

- **`WebPortal`** — serves the static portal files under `src/web/`
  (HTML / CSS / JS / icons / Workbox PWA shell).
- **`APIController`** — JSON API: `GET /api/status`, `/api/meta`,
  `/api/profiles`, …; `POST /api/streaming/{start,stop}`,
  `/api/shader`, `/api/profiles/save`, …; `WS /api/shader/preview`
  for live updates.
- **`HttpAuth`** / `PasswordHash` — stream password gating shared
  between the portal, `/stream`, and `/raw`.

**Remote source mode (#47)**

- **`RemoteMetaSync`** — when the local source type is **Remote**,
  this background worker polls (or long-polls via SSE when supported)
  the upstream `/meta` endpoint and pushes the host's
  shader/preset/parameter changes into the local UI so the client
  mirrors the host's look.

**Public directory (#49 + #53 + #60)**

- **`DirectoryClient`** — opt-in publish flow. Registers a stream
  entry against the public directory service, heartbeats every ~30 s,
  patches on parameter changes, and deletes on stop.
- **`DirectoryBrowser`** — opposite direction; lists what's currently
  published so the user can connect from the UI.
- **`CloudflaredManager`** — supervises a child `cloudflared`
  process. Two modes: Quick (`trycloudflare.com` ephemeral URL) and
  Named (user-owned hostname). Used so a publish behind CGNAT still
  produces a reachable `/raw` URL without portforwarding.
- **`CloudflaredDownloader`** — fetches the platform-appropriate
  `cloudflared` binary on first need (sha256-checked).
- **`CloudflaredAccount`** — Named-tunnel credential storage.

### `src/osd/` — On-screen overlays

- **`ConnectionStatusOverlay`** — pinned status badge for Remote
  source mode (decoded/consumed FPS, queue depth, viewer count,
  host nickname).
- **`QuickActionsOverlay`** — bottom-right floating buttons (start /
  stop stream and record, open browse window, disconnect remote)
  that stay reachable when the main UI is hidden.

### `src/ui/` — ImGui-driven UI

`UIManager` owns the configuration model (the single source of truth
the rest of the app reads from), wires shortcuts, and renders the
per-area windows from `UIConfiguration*` and the auxiliary panels
(`UIInfoPanel`, `UIRemoteConnection`, `UIDirectoryBrowser`,
`UIRecordings`, `UICredits`, `UIPreferences`, `UIShortcutsHelp`,
`UICapturePresets`). The web portal mirrors the same config model
through `APIController`, so changes in either surface stay in sync.

### `src/utils/` — Cross-cutting utilities

- **`Paths`** — XDG / Known-Folders aware path resolution for the
  assets / config / data / cache / recordings roles, with per-role
  env-var overrides.
- **`Logger`** — process-wide rate-limited logger (lock-free,
  per-thread buffers).
- **`HttpClient`** + **`HttpClientTls`** — small synchronous
  HTTP/HTTPS client. OpenSSL-backed TLS with hostname verification
  and SNI; probes well-known CA bundle paths at startup so AppImage
  binaries don't hit OpenSSL's STORE/unregistered-scheme error
  (see #69).
- **`PresetManager`** / **`ShaderScanner`** — shader discovery and
  thumbnailed preset gallery.
- **`ThumbnailGenerator`** — offline render of preset thumbnails
  used by the gallery and the web portal recordings list.
- **`TranslationManager`** — i18n (EN / PT-BR; configurable, see
  #45/#46).
- **`V4L2DeviceScanner`** + **`v4l2/V4L2ControlMapper`** — V4L2
  device enumeration and human-readable control names.
- **`FilesystemCompat`** / **`FFmpegCompat`** — small compatibility
  shims for older toolchains (notably MXE's MinGW-w64 GCC and
  pre-5.1 FFmpeg).
- **`LanCheck`** — does the local listener bind a routable
  interface? Used by the directory publish flow's "your local port
  isn't reachable from the internet" warning.

### `src/web/` — Web portal static assets

HTML + JS + CSS + PWA manifest + Workbox-based service worker.
Loaded from disk at runtime by `WebPortal` from the installed
`src/web/` tree (resolved via `Paths`).

---

## Data and threading model

### Threads

Per session, the following threads are alive (Linux numbers; Windows
is similar):

| Thread                                              | Origin                                       |
|-----------------------------------------------------|----------------------------------------------|
| Main loop (GL, ImGui, capture, push)                | `Application::run`                           |
| V4L2 capture (mmap dequeue / requeue)               | `VideoCaptureV4L2::startCaptureThread`       |
| Audio capture                                       | `AudioCapturePulse::startReadThread`         |
| Two encoder threads (`/stream` + `/raw`)            | inside `HTTPTSStreamer`                      |
| HTTP server worker pool                             | `HTTPServer`                                 |
| `DirectoryClient` heartbeat                         | when publish is on                           |
| `CloudflaredManager` supervisor                     | when tunnel mode is on                       |
| `RemoteMetaSync`                                    | when source is Remote                        |
| `VideoCaptureRemote::decodeLoop`                    | when source is Remote                        |
| Audio playback ring                                 | when source is Remote                        |

Most of these are cleanly joined on shutdown via the `unique_ptr`
chain. The Remote-source worker threads have an explicit interrupt
callback (`m_decodeAborted`) so a stuck FFmpeg `avio_read` doesn't
block process exit beyond ~1.5 s.

### Buffers and ownership

- The video texture pipeline uses one OpenGL texture per role
  (source, shader output, recording readback, streaming readback) —
  no per-frame allocation in steady state.
- The streaming and recording paths share `MediaSynchronizer`'s
  bounded audio ring and bounded frame queue. Overlap-gated A/V sync
  drops frames or pads audio when the gates open / close.
- `VideoCaptureRemote` keeps a small frame queue (default 20) with a
  drop-oldest policy under back-pressure; downstream texture upload
  happens on the main thread so the decoder thread never touches GL.

### Persistence

- **User config** (`config.json`) is loaded once at startup and saved
  on every `UIManager` change. Lives under `XDG_CONFIG_HOME` /
  `%APPDATA%`.
- **Capture presets**, **streaming profiles**, **recording profiles**
  and **recording sidecar metadata** live under
  `XDG_DATA_HOME` / `%APPDATA%\data\`.
- **Logs** go to `XDG_CACHE_HOME` / `%LOCALAPPDATA%\Cache\` so
  uninstall doesn't leave them behind.

---

## External interfaces

| Endpoint                       | Served by                | Purpose                                    |
|--------------------------------|--------------------------|--------------------------------------------|
| `GET /`                        | `WebPortal`              | Portal landing page                        |
| `GET /static/*`                | `WebPortal`              | Portal JS / CSS / icons                    |
| `GET /stream` (HTTP MPEG-TS)   | `HTTPTSStreamer`         | Post-shader live stream (portal, VLC, …)   |
| `GET /raw` (HTTP MPEG-TS)      | `HTTPTSStreamer`         | Pre-shader live stream for Remote clients  |
| `GET /meta` (JSON or SSE)      | `APIController`          | Host's current shader/preset/params snapshot |
| `GET /thumbnail/<id>.jpg`      | `WebPortal`              | Recording thumbnail                        |
| `POST /api/streaming/start`    | `APIController`          | Programmatic stream control                |
| `WS /api/shader/preview`       | `APIController`          | Live shader-parameter push                 |
| Directory `POST /register`, … | `DirectoryClient` ↔ remote | Publish + heartbeat + patch + delete       |

Note: `/raw` carries the capture in the host's canonical bottom-up
orientation (matching `/stream` and recording); the Remote client
compensates that inversion on display. See the CHANGELOG entry for the
0.8.2-alpha orientation standardization.

---

## Build matrix

Four targets are produced from the same source tree by the scripts
under `tools/`:

| Target                | Toolchain              | Output                                |
|-----------------------|------------------------|---------------------------------------|
| Linux x86_64          | host GCC               | `build-linux-x86_64/bin/retrocapture` |
| Linux ARM64 (RPi 4/5) | Debian arm64v8 docker  | `build-linux-arm64v8/bin/retrocapture` |
| Linux ARMv7 (RPi 2/3/Zero) | Debian arm32v7 docker | `build-linux-arm32v7/bin/retrocapture` |
| Windows x86_64        | MXE MinGW-w64 docker   | `build-windows-x86_64/bin/retrocapture.exe` |

All four ship with the same `src/web/`, `shaders/` and `assets/`
trees, resolved at runtime via `Paths` (XDG roles on Linux,
Known-Folders on Windows).

---

## Where to start reading

- New to the codebase → this file, then `src/core/Application.cpp`'s
  main loop, then the subsystem of interest.
- Touching streaming → `src/streaming/HTTPTSStreamer.{h,cpp}` and
  `APIController.{h,cpp}` (`/meta`).
- Touching directory publish → `src/streaming/DirectoryClient.{h,cpp}`.
- Touching shaders → `shaders/README.md` (preset format) and
  `src/shader/ShaderEngine.{h,cpp}`.
- Committing → [`CONTRIBUTING.md`](CONTRIBUTING.md).
