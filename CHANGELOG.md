# Changelog

All notable changes to RetroCapture will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- Shaders rendered nothing on the live output (window, stream, recording,
  virtual camera): the per-pass render loop exited early on an inner-loop
  guard, returning the unshaded input. Restored shader rendering (#184).
- Recording came out upside-down when the Recording tab's apply-shader
  toggle was off: the raw-source readback used the opposite vertical
  orientation from the shaded path. Recording, `/stream` and `/raw` now
  share the host's canonical orientation (#187).
- The Remote source client ignored the Streaming tab's apply-shader
  toggle and kept applying the shader; the `/meta` snapshot now reports
  the effective streaming shader state so the client follows it (#188).

### Changed

- **Remote protocol orientation (breaking):** the `/raw` feed now carries
  the capture in the host's canonical bottom-up orientation, matching
  `/stream` and recording. A 0.8.2-alpha client paired with a
  pre-0.8.2-alpha host (or vice-versa) renders the remote picture
  upside-down. Mixing versions across this boundary is unsupported (#187).

### Planned

- WebRTC streaming support (#52)
- Shader bundle fetch so a client without the host's preset still
  reproduces the host's look (#54)
- Long-session A/V drift validation (#67 follow-up) — the
  catastrophic mid-join drift is fixed; the 30 min / 4 h offset
  targets in #67's original acceptance still want a long-session
  measurement pass.

---

## [0.8.1-alpha] - 2026-06-12

Fourteenth alpha release. A focused **Windows-hardening** pass: every Windows
bug reported against 0.8.0-alpha is fixed, so capture, streaming, recording,
audio and the virtual camera all work on a fresh Windows install. No new
features — stability and platform-correctness only.

### Fixed

- **Windows stream/recording rendered a solid gray screen** (#129). The
  FFmpeg 4.1 libx264/libx265 build in the MXE toolchain corrupts the stream to
  empty frames under both ABR (`bit_rate`) and VBV rate control — VBV
  underflow a few seconds in turned the picture to gray. The Windows software
  encoder now uses pure constant-quality CRF (no ABR/VBV), which the bisection
  proved encodes correctly. Other platforms keep ABR.
- **Windows captured the wrong audio as white noise** (#137). The audio
  configuration window was never shown on Windows (so the capture card's audio
  endpoint couldn't be selected), and the WASAPI 32-bit-float mix format was
  read as int16 — white noise even with the right device. The window is now
  available, the float format is decoded correctly, and a **local audio
  monitor** (with a "Resync monitor" button) lets the operator hear the capture
  live, mirroring Linux/macOS.
- **Monochrome, horizontally-tiled capture on some devices** (#135). Cards
  delivering NV12/UYVY/RGB32 (instead of the requested RGB24) were mislabeled
  and rendered as grayscale repeated ~3× across the top of the window. The
  DirectShow grabber now converts NV12, UYVY and RGB32 to RGB alongside the
  existing YUY2 path.
- **Virtual camera never appeared in OBS/Zoom/Teams** (#133). The DirectShow
  filter registered only under the legacy filter category, never under
  `CLSID_VideoInputDeviceCategory`, so capture apps didn't list it. It is now
  registered (and unregistered) under the video-input category.
- **TLS errors listing the public stream directory on Windows** (#130) — load
  the Windows OS trust store (ROOT + CA) into OpenSSL.
- **Installer created non-working shortcuts** (#131) — fixed the Start Menu
  entry and added a Desktop shortcut.
- **ImGui window positions weren't kept across runs** (#132) — persist the
  layout on shutdown.

### Changed

- The "no device" capture placeholder is now a calm dark gray instead of a
  jarring bright green (#134).

---

## [0.8.0-alpha] - 2026-06-10

Thirteenth alpha release. A platform-reach release: the first working
**macOS x86_64 port**, a **system tray / background-operation** mode, a
**virtual camera** output, **screen capture** and **system-audio**
sources, **real-time chat**, and a Linux **audio pipeline refactor** —
plus a deep round of remote-client A/V-sync and streaming-stability
hardening that makes distributed playback hold sync over long sessions.

**Compared to 0.7.0-alpha**: macOS port (#18), system tray + minimize-to-
tray (#86), virtual camera (#85), screen-capture source (#108), system-
audio capture (#109), chat v0.5 (#88), Linux audio source/sink refactor
(#78), client-side stream volume (#77), and a streaming/A-V-sync fix wave
(remote connection lifecycle, shared-epoch sync, reconnect-to-live,
chunked `/meta`, dual-encode gating).

### Changed

- **Linux audio: drop null-sink loopback for a coherent `RetroCapture`
  source/sink pair** (#78) — the host pipeline used to load a
  `module-null-sink sink_name=RetroCapture` and route the capture-card
  audio into it via `module-loopback` so we could record the sink's
  `.monitor`. The user had to understand the PulseAudio sink/source
  distinction and route an app via `pavucontrol`. Replaced by:
  - A direct `PA_STREAM_RECORD` against the user-picked capture
    device. No more null-sink.
  - A new in-process `AudioBus` fan-out so multiple consumers
    (encoder/recorder, the published source, the local monitor) all
    pull from the same owned audio pipeline — opens the door for a
    DSP chain follow-up without rearchitecting.
  - A `module-pipe-source source_name=RetroCapture` that publishes
    what RetroCapture is currently capturing to the OS audio graph,
    so other apps (DAW, monitoring chain) can record from it — the
    "input" half of the symmetric pair.
  - A host-side `MonitorPlayback` (`pa_simple` PLAYBACK stream named
    `RetroCapture` to the default sink) — the "output" half of the
    pair, mirror of the client's `AudioPlaybackPulse`. Picking a
    device under the Audio tab is now audible immediately, with no
    `pavucontrol` routing. Kept in-process (no module-loopback) so
    a future DSP chain can sit between the bus and the playback.
  - A **"Resync monitor" button** in the Audio tab (native + web
    portal) that drains the monitor backlog and flushes PulseAudio's
    playback buffer, snapping the monitor back to ~50 ms latency.
    Useful when a stall accumulates a delay between the captured
    audio and what the user hears.
  - Audio tab copy across native UI and the web portal updated to
    "Input device" / "Start capture" / "Stop capture" and shows a
    live status line with the current sample rate / channels.
  - A one-shot migration GC that unloads any leftover pre-0.8
    `module-null-sink sink_name=RetroCapture` + matching
    `module-loopback` modules at startup, so upgrades from 0.7.x
    don't leave stale modules behind.
- Removed ~700 lines of dead `module-null-sink` / `module-loopback`
  scaffolding from `AudioCapturePulse`, plus the `restoreAudio-
  DeviceConnections` two-step (the saved input source is now passed
  directly to `AudioCapturePulse::open()` during init).

### Added

- **Real-time chat with identity-bound rooms** (#88) — a chat service
  (`platform/services/chat/`, Go + WebSocket) plus an in-app client:
  each live stream gets a room bound to the streamer's identity, with
  an OSD overlay for the host/viewers and a panel in the web portal.
  Lets a streamer and remote viewers talk without a third-party tool.
  (v0.5 — moderation / reactions / slash commands tracked for a later
  release under #87.)
- **Virtual camera output** (#85) — RetroCapture can expose its
  shader-processed output as a system virtual-camera device, so the
  CRT-filtered picture can be selected as a webcam in Zoom / OBS /
  browsers. `v4l2loopback` on Linux, the DirectShow/Media-Foundation
  virtual cam on Windows, and a CoreMediaIO path on macOS.
- **Screen capture source (monitor / window / region)** (#108) — a
  third capture source alongside V4L2/AVFoundation and remote: grab a
  whole monitor, a single window, or a dragged region. Backends per
  platform — PipeWire (xdg-desktop-portal) on Linux/Wayland, the
  Windows graphics capture path, and `ScreenCaptureKit` on macOS.
- **System-audio output capture** (#109) — capture what the machine is
  *playing* (the monitor / loopback of the default output) as an audio
  source, in-process via the `AudioBus`, so a desktop-audio stream
  carries sound without routing tricks. On macOS this uses the
  `ScreenCaptureKit` audio path; a display filter is required for SCK
  to deliver buffers.
- **Client-side volume control for the remote stream** (#77) — a
  linear gain slider applied to the incoming remote audio just before
  the sink, live from the UI / quick-actions widget, persisted across
  runs. The playback clock math is unaffected so A/V sync holds.
- **System tray + minimize-to-tray for background operation** (#86) —
  RetroCapture can now hide its window and keep every pipeline
  (capture, shader, streaming, recording, virtual camera, chat)
  running in the background while a tray icon stays in the system
  tray. A new cross-platform `ISystemTray` abstraction (`src/tray/`)
  with native backends:
  - **Linux**: StatusNotifierItem over D-Bus (no GTK dep — drives
    libdbus directly). Native on KDE/Plasma, XFCE, Cinnamon, MATE;
    GNOME needs the AppIndicator extension. Serves the bundled logo
    as the SNI IconPixmap so the branded icon shows on any desktop
    without a themed-icon install.
  - **Windows**: `Shell_NotifyIcon` + `TrackPopupMenu` (no new deps).
  - **macOS**: `NSStatusItem` + `NSMenu`.
  - Context menu: Start/Stop Streaming, Start/Stop Recording, Open
    Web Portal, Show/Hide Window, and **Quit** (orderly teardown).
    Items grey out when the matching pipeline isn't ready.
  - The window close button minimizes to tray (configurable) instead
    of quitting; the render loop skips the viewport swap while hidden
    (a hidden window's swap can block on a parked compositor vsync)
    and paces itself so the pipelines keep feeding consumers.
  - Falls back cleanly to quit-on-close when the desktop has no tray
    host, with a one-line warning.
  - Desktop notifications on streaming/recording start/stop/saved,
    gated by a preference — native per platform (freedesktop
    Notifications over D-Bus on Linux, Shell_NotifyIcon balloon on
    Windows, NSUserNotification on macOS), no extra deps.
  - New Preferences → System tray section: show tray icon, minimize
    on close, start minimized, show notifications (all persisted).
- **macOS x86_64 port — first working build of host + client modes**
  (#18). Cherry-picked the older `18-port-to-macos-13-or-later-x86_64`
  branch's backend files onto current 0.8.0-alpha and rebuilt the
  user-facing surface in 0.8 idioms. Highlights:
  - Video capture via `VideoCaptureAVFoundation` with the runtime
    camera-permission probe pattern (`AVAuthorizationStatusForMedia-
    Type:AVMediaTypeVideo`) so the dropdown is no longer empty
    silently. Format dropdown is OBS-style (resolution + fps range +
    pixel format picked atomically per device). Format changes do a
    close+reopen because macOS reverts `activeFormat` mid-session;
    output dimensions are forced via `videoSettings`'
    `kCVPixelBufferWidthKey/HeightKey` because
    `AVCaptureSessionPresetInputPriority` is iOS-only on the macOS
    SDK. Framerate clamping via `AVFrameRateRange.minFrameDuration`
    (with `@try/@catch` belt-and-suspenders) prevents
    `NSInvalidArgumentException` on devices that only support
    rational rates like 29.97.
  - Audio capture via `AudioCaptureCoreAudio` mirroring Linux's
    `AudioCapturePulse` architecture: same `AudioBus` fan-out, a
    local tap backing `getSamples`, plus an in-class monitor
    playback (`AudioOutputCoreAudio` + writer thread) — the macOS
    counterpart of `MonitorPlayback`. AudioUnit gets bound to the
    user-selected device via `kAudioOutputUnitProperty_CurrentDevice`
    BEFORE format negotiation, then adopts the device's native
    sample rate / channel count so UVC cards that run at 48 kHz /
    mono / float32 don't fail with
    `-10863 kAudioUnitErr_CannotDoInCurrentContext`. Microphone
    permission probe lives at the top of `open()`.
  - Client-mode remote-stream playback via the new
    `AudioPlaybackCoreAudio` (adapts `IAudioPlayback`'s float-PCM +
    PTS API onto `AudioOutputCoreAudio`'s int16 push).
  - `MediaEncoder::convertRGBToYUV` clamps odd source heights to the
    nearest even value before `sws_scale` — macOS window framebuffer
    (1080 minus menu bar ≈ 953) is odd, and libswscale 9.x's AVX2
    fastpath would `EXC_BAD_ACCESS` on the unaligned tail.
  - Auto-open of the saved AVFoundation device + format on
    `SourceType::AVFoundation` activation, so the first frame after
    launch is the same capture the user had in the previous session.
  - Native UI + web portal expose AVFoundation device / format and
    Core Audio input device pickers; the `Resync monitor` button
    works on macOS too.
- See `docs/MACOS_PORT_STRATEGY.md` for the full inventory of
  resolved gotchas and the follow-up list (`.app` bundle with
  `Info.plist` permission strings, codesigning, macOS source
  publisher equivalent of `module-pipe-source`, Apple Silicon
  build, CI/release integration).

### Fixed

- **Remote-client connection lifecycle** (#92, #95, #100) — disconnect
  no longer freezes the UI, the reconnect counter is accurate, and a
  long-GOP stream connects cleanly instead of stalling on the first
  keyframe wait.
- **Non-blocking connect + non-freezing disconnect** (#106) — the
  remote connect/disconnect now runs off the UI thread with live
  state feedback in the connection overlay.
- **AAC mid-join audio** (#102) — when a client joins mid-stream and
  the AAC probe can't resolve channel/rate yet, the audio sink is
  deferred to the first decoded frame instead of opening with bad
  params (which had silenced audio on reconnect).
- **Remote A/V sync over reconnects and long sessions** (#93, #109) —
  a shared A/V epoch (video and audio made relative to one origin so
  the picture stops lagging the sound), a jump-to-live drain on
  (re)connect that drops the stale backlog, an audio anchor gate that
  refuses stale pre-anchor audio (fixed the ~-14 s reconnect desync),
  /raw send backpressure that drops backlog instead of dropping the
  connection, and gating the `/stream` encode so it idles when nobody
  is watching it. Net: from "-14 s reconnect desync, no audio,
  connection cascade" to a stable ~0 to -50 ms audio-locked window.
- **Stream-switch crash on Linux** (#112) — ignore `SIGPIPE` on Linux
  too, so switching streams while a socket write is in flight no
  longer takes the process down.
- **Local capture rebuild on source switch** (#97) — switching back to
  a local V4L2 source from Remote/Screen rebuilds the capture backend
  instead of reusing a torn-down one.
- **`/meta` JSON robustness** (#99, #113) — emit valid JSON for
  non-finite shader-parameter floats (NaN/Inf no longer corrupt the
  payload), and announce the real screen-capture dimensions so the
  client renders the shader at the host's logical source size.
- **`/meta` SSE behind a proxy** (#120) — the SSE reader now decodes
  chunked `Transfer-Encoding`, so shader/parameter/source sync works
  through a relay (e.g. Cloudflare) that re-frames the response —
  previously every client logged a stream of JSON parse failures.
- **`/stream` no longer encodes for `/raw`-only clients** (#123) — the
  "idle the /stream encoder when unwatched" gate counted `/raw`
  subscribers via the combined audience count, so a single remote
  `/raw` client made the host run a second 720p60 VAAPI encode that
  overflowed the `/stream` synchronizer and stole GPU from `/raw`.
- **Web recordings playback + live player auto-recovery** (#79, #80) —
  the recordings page plays back reliably and the live web player
  recovers from a transient stream hiccup instead of staying black.
- **Reap stale audio module on startup** (#96) — a crash that strands
  the `module-pipe-source source_name=RetroCapture` (and its FIFO) is
  now cleaned up on the next launch, so RetroCapture sources don't
  accumulate across crash-restart cycles.
- **Windows cross-build** (#89) — unbroken after the chat (#84) merge.

### Known issues

- **Remote `/raw` connection drops through a relay** (#122) — over a
  bandwidth-limited relay path the host can't always push 720p60 fast
  enough; the relay then cuts the read side and the client reconnects.
  Mitigated by the dual-encode gating in #123; a direct (LAN) or
  higher-upload path is stable. Adaptive bitrate is tracked for a
  follow-up.
- **Residual progressive A/V drift on the remote client** (#124) — on
  a long, stable connection the playback slowly falls behind live
  (~0.5 ms/s, audio stays locked to video) due to a host
  audio-capture-clock vs video-clock mismatch, bounded by a periodic
  re-anchor to the live edge. A proper host-side re-clocking fix is
  deferred past this alpha.

### Scope

- Linux audio refactor (#78) is Linux-only — WASAPI loopback already
  works differently on Windows; the host audio path on Windows is
  untouched in this release.
- macOS port (#18) lands x86_64 only and depends on Homebrew FFmpeg /
  GLFW / libpng; build via `tools/build-macos.sh` on the Mac itself
  (no cross-compile from Linux).

---

## [0.7.0-alpha] - 2026-05-21

Twelfth alpha release. Two large product surfaces (shader-preserving
distributed playback and the public stream directory) plus the
recording / UI shell / networking polish that made the alpha
shippable to people other than the maintainer.

**Compared to 0.6.0-alpha**: 12 PRs · shader-preserving distributed
playback, public stream directory + Cloudflare tunnels, HTTPS / TLS
everywhere, bilingual UI, unified recording UI idiom, on-screen quick
actions overlay, plus a wave of client-side UX fixes.

### Added

- **Shader-preserving distributed playback** (#47, PR #50) — a
  RetroCapture instance can now consume another's `/raw` MPEG-TS
  feed as a video source. The host also exposes a `/meta` side
  channel carrying the current preset, parameter values, brightness
  / contrast and source dimensions; the client mirrors all of it
  through `RemoteMetaSync` so the picture renders with the same
  shader output on both machines. SSE long-poll for `/meta` keeps
  parameter edits live; PTS-anchored playback timing absorbs network
  jitter. Wire format documented in
  `docs/REMOTE_STREAM_PROTOCOL.md`.
- **Public stream directory** (#49, PR #55) — opt-in listing of
  live streams so a client can browse + connect without out-of-band
  URL sharing. New `platform/services/directory/` Go service
  (SQLite, embedded migrations, token-bucket rate limits, reaper),
  host-side `DirectoryClient` lifecycle, dedicated Browse window
  under **Remote → Browse public directory…**, `--browse-directory`
  CLI flag. Live at `http://directory.retrocapture.com`.
- **Integrated Cloudflare Quick Tunnel** (#49 Phase 2.5, PR #55) —
  `cloudflared` spawned + supervised by the host; the random
  `trycloudflare.com` URL is auto-registered with the directory.
  Lets users behind NAT/CGNAT publish without port forwarding or
  buying a domain.
- **Auto-download of `cloudflared`** (#53, PR #61) — first-time use
  of the tunnel option downloads the pinned binary (`2026.5.0`) from
  GitHub releases, verifies sha256, caches under
  `<user-data-dir>/cloudflared/`. No manual install. ARM32 hidden
  because there's no upstream binary there. CLI escape hatch
  `--cloudflared-binary <path>` for air-gapped setups.
- **Named Cloudflare Tunnel** (#60, PR #62) — persistent
  shareable URL tied to the user's own domain. Sign-in to
  Cloudflare from inside the app via `cloudflared tunnel login`
  (the OAuth URL is rendered in a copy-pasteable read-only
  InputText for headless / SSH users); tunnel selection /
  creation / DNS routing all from the same screen.
- **Stream password gate** (#49 Phase 3, PR #55) — host's entire
  HTTP surface (web portal HTML, vendor assets, PWA, `/stream`,
  `/raw`, `/meta`) now sits behind a single password when
  configured. Two accepted schemes: Basic (browsers) and
  Bearer-sha256 (native client). Documented in
  `docs/DIRECTORY_PROTOCOL.md`.
- **Report a stream** (#57, PR #63) — right-click → Report on any
  row in Browse opens an input modal; submit fires
  `POST /streams/<id>/report` and a feedback modal returns the
  protocol number `R-XXXXXXXX` for the user to quote later. Receipt
  persisted in `stream_reports.report_id` (migration 002) so the
  maintainer can pivot from a quoted receipt back to the row.
- **Capped exponential backoff on remote reconnect** (#58, PR #63) —
  `VideoCaptureRemote::decodeLoop` retries follow
  `[2, 2, 5, 5, 15, 30, 60]` seconds with the 60 s slot held for
  prolonged failures. After ~15 minutes of failed retries the UI
  surfaces "Host appears offline" alongside an OSD overlay (see
  below); URL stays armed.
- **Always-on-top connection state overlay** (PR #63) — bottom-right
  corner shows Connecting / Reconnecting / Disconnecting / Connected
  transitions, visible even with the rest of the UI hidden via F12.
- **F11 toggles fullscreen** (PR #63).
- **Mode-aware Info tab** (PR #63) — host mode keeps Capture +
  Streaming sections; client mode renders dedicated Remote Stream +
  Connection blocks instead of mislabelling the remote URL as a
  "Device".
- **Client URL validation** (#56, PR #63) — Custom endpoint URL
  field rejects malformed inputs (scheme, host, port) inline before
  the publish toggle goes green. Same validation on the service in
  `validateRegister` / `validatePatch`. 8 new Go tests cover the
  rejection cases.
- **Cloudflare Quick Tunnel DNS propagation hint** (PR #63) —
  inline note under the directory status text warning that
  `trycloudflare.com` URLs can take ~2 min to resolve from other
  networks after publish.
- **Bilingual UI: English + Portuguese** (#45, PR #64 / #65) —
  runtime language switch from Preferences, full per-string
  translation table for the native UI, the web portal and CLI
  `--help`. The startup-language preference persists across runs.
- **Preferences pane** (parts of #45 / #46, PR #64 / #65) —
  dedicated Preferences window with language and default-fullscreen
  toggles, separated from the per-area Configuration tabs.
- **Recording pipeline parity + unified UI idiom** (#59, PR #66) —
  recording and streaming now share `MediaSynchronizer` semantics
  (overlap-gated A/V sync, 50 ms tolerance) so they no longer
  diverge under load. Every Configuration tab adopts a single layout
  idiom: section header → controls → status footer. Output
  resolution, codec, bitrate and profile widgets normalised across
  both pipelines.
- **Quick Actions OSD** (#68, PR #72) — bottom-right floating
  overlay with Stream / Record / Browse / Disconnect buttons that
  stays reachable when the main UI is hidden. Adapts to whether the
  app is currently host or client.
- **Shortcuts helper widget** (#68, PR #72) — top-right
  always-visible reference of the active keyboard shortcuts.
- **OSD layer split** (#68, PR #72) — `src/osd/` is now a separate
  source root for pinned overlays, distinct from the interactive
  `src/ui/` windows.
- **Combined `/stream` + `/raw` client count** (#68, PR #72) — the
  `/meta` payload now exposes a single `streaming.clientCount`
  that reflects the union of both endpoints, surfaced in the
  Quick Actions widget and the Connection Status overlay.
- **TLS / HTTPS support in HttpClient** (#69, PR #73 / #74) —
  OpenSSL-backed TLS with hostname verification + SNI. Directory
  publish, directory browse, `/raw` consumption and `/meta`
  long-poll all default to `https://`. New `HttpClientTls.h`
  shared header so SSE goes over TLS too. Process-wide CA-bundle
  probe (env, well-known paths, then OpenSSL defaults) keeps
  AppImage builds working without `error:16000069:STORE routines::
  unregistered scheme`.
- **`directory.retrocapture.com` over HTTPS** — defaults flipped
  throughout (CLI help, `--directory-url` default, the publish
  Advanced field).
- **Allow self-signed TLS cert toggle** (#69, PR #74) — Advanced
  dev option for testing against self-signed directory hosts;
  off by default and never persisted as on for the public host.
- **Directory URL hot-reload** (#69, PR #74) — changing the
  Directory URL in the UI re-registers the publish and restarts
  the browser worker immediately instead of waiting for an app
  restart.

### Changed

- **Repo-wide English string normalization** (#46) — the web
  portal HTML/JS, ImGui labels and tooltips, `--help` text, and
  user-visible `LOG_*` messages were all standardized to English.
  Pre-existing Portuguese strings remained from earlier alphas;
  cleaned up alongside #45's runtime language switch so the
  default-English path and the PT-BR translation pull from the
  same canonical key table. Stale `RetroCapture v0.5.0` literal
  in the Credits window and main startup log replaced with the
  `RETROCAPTURE_VERSION` macro CMake propagates from `project()`.
- **TrustProxyHeaders flag on the directory service** — when the
  service is deployed behind FRP / Cloudflare / nginx, set
  `DIRECTORY_TRUST_PROXY_HEADERS=true` so the recorded
  `publicIp` / `reporter_ip` is the real client, not the proxy.
  Off by default for direct-exposure deployments (spoofing-
  resistant). Same lookup feeds the rate limiter so one proxy IP
  doesn't burn the bucket for everyone behind it.

### Fixed

- **Web portal stuck loading vendored assets via Cloudflare** —
  `/stream` was substring-matching `Referer: https://stream.retrocapture.com/`
  and serving MPEG-TS bytes for every asset (PR #55).
- **HTTP/1.1 RST instead of FIN on connection close** —
  `shutdown(SHUT_WR)` + bounded drain before `close()` (PR #55).
- **Partial sends silently truncated the MPEG-TS stream**, making
  mpegts.js lose the SourceBuffer permanently after minutes of
  viewing through a tunnel. Per-client tail buffer preserves byte
  order; client closed cleanly past a 4 MB backlog cap (PR #55).
- **`SO_RCVTIMEO` leaking into the stream-serve phase** caused MSE
  `onSourceEnded` after 5 s (PR #55).
- **Configuration tab leaking externally behind FRP** — peer-IP
  detection augmented with a header-based `cameFromInternetProxy()`
  sniff (PR #55).
- **`RemoteMetaSync` failing over HTTPS tunnels** — routed through
  FFmpeg `avio_open2` so libavformat handles TLS (PR #55).
- **PulseAudio `pthread_mutex_destroy != 0` after hours of
  uptime** — `std::shared_mutex` around `m_stream` in
  `AudioPlaybackPulse` so `close()` can't race with
  `submit()` / `getClockUs()` (PR #55).
- **`stream` overlay shown only when UI was visible** —
  `UIManager::endFrame` now always calls `ImGui::Render` so the
  overlay paints even with F12 hiding the main UI (PR #63).
- **OSD overlay missed killed hosts** — added an
  `isReceivingFrames()` heuristic on `VideoCaptureRemote` that
  combines `m_streamAnchored` with a `m_lastFrameAtSteadyUs`
  staleness check (2 s threshold) — killed host shows up in the
  overlay within ~2 s instead of waiting on the TCP timeout (PR #63).
- **DirectShow device dropdown leaked the remote URL** — when the
  user switched source from Remote to DirectShow the dropdown
  rendered `http://localhost:8080` as if a webcam was at that
  address. Now falls back to "None (No device)" when the stored
  value isn't a DS or V4L2 device (PR #55).
- **`AV_NOPTS_VALUE` poisoning the audio clock on mid-join** (#67,
  PR #75) — `VideoCaptureRemote` substituted `0` for an unset audio
  PTS, anchoring the audio clock at "stream-time 0" while video
  sat at the server's current uptime. Produced multi-hundred-second
  drift warnings (`audio clock drift=-221_887_834us — falling back
  to wall clock for A/V sync`) on every mid-join.
- **AAC probe race on mid-join** (#67, PR #75) —
  `avformat_find_stream_info` was giving up before seeing enough
  AAC packets, resulting in `Audio: aac, 0 channels: unspecified
  sample format` and `AudioPlaybackPulse::open — invalid format`
  whenever the client landed mid-PMT cycle. Probe budget bumped
  from 256 KB / 1 s to 512 KB / 2.5 s.
- **`0x0` video probe accepted then dead-locked** (#67, PR #75) —
  on an unstable TLS reconnect the demuxer occasionally returned
  the video stream with `0x0` dimensions. `initDecoder` now
  rejects those and forces another reconnect, so the consumer no
  longer hangs at `decoded=60/s consumed=0/s drops=60`.
- **`/raw` stopped transmitting when the host shader was off**
  (#67, PR #75) — the source-frame capture gate required
  `masterOn && shaderActive`, silently killing `/raw` video while
  audio kept flowing. Widened to fire whenever `/raw` has clients.
- **Ghost audio after Disconnect** (#67, PR #75) —
  `AudioPlaybackPulse::close()` swaps `pa_simple_drain` for
  `pa_simple_flush` so the queued samples are discarded
  immediately on disconnect instead of playing out for several
  seconds.
- **Remote source upside-down without a client-side shader** (#67,
  PR #75) — the renderer's default `flipY=true` overshoots when
  the shader chain isn't there to implicitly compensate. Now
  conditionally dropped for `Remote && !isShaderTexture`.
- **Windows installer: "shaders not found" at startup** —
  `Paths::getReadOnlyAssetsDir()` was only probing
  `<exeDir>/assets` and `Program Files/RetroCapture/assets`, both
  assuming a layout where every asset bucket lives inside an
  `assets/` subfolder of the .exe. The NSIS / CPack installer
  actually puts the .exe in `<install>/bin/` and shaders / web /
  ssl / assets in `<install>/share/retrocapture/`, so the lookup
  missed every bucket and the app started with no shaders, no web
  portal and no TLS cert. Rewritten to probe by `shaders/
  shaders_glsl` presence in the build-tree layout, then the
  installer's `share/retrocapture/` layout, then `Program Files`.
  Affects only the Windows shipped binary; Linux uses the
  XDG_DATA_DIRS resolution path which already covered both
  layouts.

### Deferred

- **Phase 2.5b** — Named Tunnel persistent-URL flow shipped as
  #60 / PR #62, separate from the auto-download work in #53.
- **Phase 4b** — shader bundle fetch (#54) is the next-step
  follow-up that closes the loop "client picks a stream from the
  directory → reproduces the host's look fully". The maintainer
  judged that bundled standard shaders cover the common case for
  this alpha and decided to defer.

---

## [0.6.0-alpha] - 2026-05-07

Eleventh alpha release. Big reliability pass on the recording / streaming
pipeline, a major web portal overhaul, named profiles for both streaming and
recording, and final bits of RetroArch GLSL spec coverage.

**Compared to 0.5.0-alpha**: 11 PRs · 76 files changed · +9,732 / −2,496 lines.

### Added

- **Recording profiles** (#39) — save / load / delete / apply complete
  recording settings (codec, bitrate, FPS, shader-apply flag, audio…) by
  name. Stored as JSON under
  `~/.local/share/retrocapture/recording_profiles/` (Linux) /
  `%APPDATA%\RetroCapture\data\recording_profiles\` (Windows). Managed
  from both the native UI and the web portal.
- **Streaming profiles** (#42) — same UX as recording profiles, parallel
  storage under `streaming_profiles/`.
- **Web portal overhaul** (#43)
  - Three dedicated pages with shared top navigation: **Home** (live
    stream player + status), **Recordings** (browse / delete / download),
    **Configuration** (everything else).
  - **Live MPEG-TS player** on Home — vendored `mpegts.js` with native
    fallback.
  - **Bootstrap + Bootstrap Icons fully vendored** — portal works fully
    offline via service worker (`v3`).
  - **Master shader pipeline toggle** — bypass the active preset without
    losing it.
  - **Per-pipeline shader override** — stream and recording can
    independently opt out of the shader, taking the source frame instead
    of the processed one.
  - **Editable preset shader parameters** via modal (was native-UI-only).
  - **Source-tab parity** — V4L2 retro-console preset row matches native
    UI; duplicated hardcoded V4L2 controls block was removed.
  - **Overscan controls** exposed in the web UI.
- **Shader spec coverage** (#29)
  - Implemented **PassFeedback** ping-pong textures.
  - Implemented the **OriginalHistory** alias chain and binding of passes
    by alias.
  - More upstream RetroArch GLSL presets now load correctly.
- **Storage path standardization** (#32)
  - Linux follows **XDG Base Directory** (config / data / cache / videos).
  - Windows follows **Known Folders** (`SHGetFolderPath` for Roaming
    AppData, Local AppData, My Videos).
  - Per-getter env overrides for AppImage / CI / packagers:
    `RETROCAPTURE_CONFIG_DIR`, `_DATA_DIR`, `_CACHE_DIR`, `_ASSETS_DIR`.
  - One-shot idempotent migration of legacy
    `~/.config/retrocapture/assets` → `~/.local/share/retrocapture/`,
    marker file `MIGRATED.txt` prevents re-runs.
- **CPU compatibility mode for distributed binaries** (#20, issue #19)
  - `BUILD_COMPATIBLE_X86_64` CMake option — applies `-march=x86-64-v2`
    baseline (or the equivalent `-march=x86-64 -msse4.2 -mno-avx -mno-avx2`
    on toolchains older than GCC 11 / Clang 12). Removes AVX/AVX2 so the
    binary runs on CPUs like the AMD A8 PRO-7600B (Kaveri) that motivated
    the issue, while still covering every Intel CPU since Nehalem (2008).
  - `BUILD_COMPATIBLE_ARM64` CMake option — applies `-march=armv8-a`
    baseline (no SVE / crypto / dotprod), so binaries built inside the
    `arm64v8` Docker image under qemu run on real Pi 4/5 hardware.
  - The same flags are applied to the bundled ImGui static library to
    keep the final binary single-baseline.
- **CPU diagnostic and dependency tooling**
  - `tools/diagnose-cpu-compatibility.sh` — inspects the instruction set
    extensions a binary actually uses (AVX, AVX2, AVX-512, SSE4.2),
    compares them with the current CPU and prints a verdict.
  - `tools/check-dependencies.sh` — verifies the shared libraries an
    executable links against.
  - `tools/install-deps-manjaro.sh` — automated dependency installation
    for Manjaro / Arch Linux.
  - `tools/clean-build.sh` — cleans all per-architecture build directories.
- **Release packaging tooling**
  - `tools/package-release.sh` — orchestrator that runs every per-platform
    build script and emits final artifacts under `dist/` together with a
    `SHA256SUMS` file. Supports `--skip-*` and `--only-*` flags so any
    subset of platforms can be rebuilt independently.
  - All four build scripts (AppImage x86_64, Linux ARM64, Linux ARM32v7,
    Windows installer) now produce artifacts under `dist/` with a unified
    naming scheme: `RetroCapture-<version>-alpha-<platform>-<arch>.<ext>`.
  - ARM64 / ARM32 Docker builds package the binary plus `shaders/`,
    `web/`, `assets/`, `ssl/`, `README.md` and `LICENSE` into a tarball
    (`RetroCapture-0.6.0-alpha-linux-{arm64v8,arm32v7}.tar.gz`).
- **Reference documentation**
  - `docs/CPU_COMPATIBILITY.md` — describes the portable baseline for
    each architecture, the compile options, the per-script defaults and
    the diagnostic workflow.

### Changed

- **Docker build scripts now default to compatible mode** so the binaries
  they produce are portable. Native (`-march=native`) builds are still
  available, but as an explicit opt-in:
  - `tools/build-linux-x86_64-docker.sh Release OFF`
  - `tools/build-windows-x86_64-docker.sh Release OFF`
  - `tools/build-linux-arm64v8-docker.sh Release --native`
- **Windows cross-compile honours `BUILD_COMPATIBLE_X86_64`** — the MXE /
  MinGW toolchain inherits `-march=native` from the build host CPU and
  would otherwise bake AVX/AVX2 into `retrocapture.exe`.
- **Streaming hardening** (#41)
  - Audio drains independently and is no longer gated by the sync zone.
  - TCP send is non-blocking (`MSG_DONTWAIT`).
  - Client back-pressure handling simplified.
  - No more audio dropouts during long streaming sessions.

### Fixed

- **Recording desync at higher quality presets** (#38). Root cause was
  PTS truncation from `time_base = {1, fps}`; bumped to `{1, 90000}` so
  PTS granularity matches the encoder's expectations. `medium + 12 Mbps`
  and above now stay in sync end-to-end.
- **Recording A/V desync, backpressure and shutdown stability** (#28).
  Backpressure across the encoding stage; clean shutdown when stopping
  mid-encode; bounded `stopRecording` join with a 5-second deadline;
  buffer overflow drops surfaced from `MediaSynchronizer`; `m_desync
  FrameCount` wired to PTS retrocession events.
- **Render pipeline latency** (#27). Tightened `glFinish` plus sync
  `glReadPixels` polling to reduce frame-to-encode latency; capture
  buffers reused across frames.
- **Capture presets now actually persist shader parameter overrides**
  (#33). Previously the overrides were re-loaded from the preset file
  every time, silently discarding any edits. Also fixes recording
  thumbnails being saved upside down.
- **V4L2 capture survives USB device disconnection.** Detect EBADF /
  ENODEV / EIO returned from any V4L2 ioctl, close the file descriptor,
  release mmap'd buffers and fall back to the internal dummy frame
  generator instead of crashing or spamming the log on every frame.
  Validates `buf.index` against `m_buffers.size()` before dereferencing
  to avoid an out-of-bounds read after a stale dequeue.
- **Main loop survives window invalidation.** Re-checks the window
  pointer and `shouldClose()` flag around `pollEvents` and `swapBuffers`
  so KVM switching, monitor hot-plug or an X server hiccup no longer
  segfaults the application. The SDL backend now handles
  `SDL_WINDOWEVENT_CLOSE` / `HIDDEN` / `EXPOSED` / `FOCUS_LOST` and
  detects an invalid window via `SDL_GetWindowFlags == 0`. The GLFW
  backend installs a SIGSEGV handler around `glfwDestroyWindow` during
  shutdown using `sigsetjmp` / `siglongjmp` to survive faulty driver
  teardown.
- **Audio capture shutdown is exception-safe.**
  `AudioCapture::stopCapture()` and `close()` are wrapped in try blocks
  during `Application::shutdown()`, so an exception from PulseAudio /
  WASAPI during teardown no longer aborts the rest of the cleanup
  sequence.
- **Shader-system polish**: PassPrev<N> {Texture, Input, Output}Size
  uniforms now bind correctly; `srgb_framebuffer` toggles
  `GL_FRAMEBUFFER_SRGB` per pass; alpha blending is disabled when drawing
  shader output to screen; `#pragma parameter` regex accepts negative
  numbers; `#include` is processed only at start of line; `PARAMETER
  _UNIFORM` is only defined when the shader actually has a `#pragma
  parameter`; texture entries are parsed before pass-index extraction;
  false-positive "no input texture" warning suppressed; pal-singlepass
  FIRTAPS array-size compile error fixed.
- **Build script repo-root detection.**
  `tools/build-windows-installer.sh` was `cd`-ing into `tools/` instead
  of repo root, causing the "run from project root" guard to fire even
  when invoked correctly.

### Distribution artifacts

| Platform | Artifact |
|---|---|
| Linux x86_64 | `RetroCapture-0.6.0-alpha-linux-x86_64.AppImage` |
| Linux ARM64 (Pi 4/5) | `RetroCapture-0.6.0-alpha-linux-arm64v8.tar.gz` |
| Linux ARM32v7 (Pi 3) | `RetroCapture-0.6.0-alpha-linux-arm32v7.tar.gz` |
| Windows x86_64 | `RetroCapture-0.6.0-alpha-windows-x86_64-Setup.exe` |

A `SHA256SUMS` file is published alongside the binaries.

### Upgrade notes

- Existing config in `~/.config/retrocapture/config.json` keeps working.
- First run of 0.6.0 migrates legacy `assets/` and `ssl/` from
  `~/.config` to `~/.local/share/retrocapture/` (Linux) /
  `%APPDATA%\RetroCapture\data\` (Windows). Idempotent — marker file
  `MIGRATED.txt` prevents re-runs.
- Existing capture presets keep loading. Re-save them once if you want
  the current shader-parameter overrides to stick (was the bug fixed in
  #33).

### Pull requests landed

- #20 — feat: CPU compatibility mode for x86-64 builds
- #27 — perf: render pipeline latency (glFinish, sync glReadPixels)
- #28 — fix/perf: recording A/V desync, backpressure, shutdown stability
- #29 — feat: close RetroArch GLSL shader spec gaps (PassFeedback, OriginalHistory)
- #32 — chore: standardize storage paths (XDG / Known Folders)
- #33 — fix: capture presets persist shader parameter overrides
- #38 — fix: recording desync at higher quality presets
- #39 — feat: recording profiles
- #41 — chore: streaming audit and harden
- #42 — feat: streaming profiles
- #43 — feat: web portal overhaul

---

## [0.5.0-alpha] - 2026-01-15

### Added

- **Video Recording System**: Complete video recording feature with extensive configuration options

  - **Recording Manager**: Full-featured recording system (`RecordingManager`)
    - Real-time video and audio recording to local files
    - Support for multiple video codecs (H.264, H.265, VP8, VP9)
    - Support for multiple audio codecs (AAC, MP3, Opus)
    - Multiple container formats (MP4, MKV, AVI)
    - Configurable resolution, frame rate, and bitrates
    - Optional audio recording with synchronization
    - Dedicated encoding thread for non-blocking recording
    - Media synchronization using `MediaSynchronizer`
  
  - **Recording UI**: Complete recording interface in both local and web UIs
    - **Local UI (ImGui)**: Dedicated recording tab with full configuration
      - Real-time recording status (active/inactive, duration, file size, filename)
      - Resolution and FPS selection (with presets or match capture)
      - Codec selection and codec-specific options
      - Bitrate controls with visual sliders
      - Container format selection
      - Output path and filename template configuration
      - Start/Stop recording button with visual feedback
    - **Web Portal**: Full recording control through web interface
      - Recording configuration and management
      - Recording list with thumbnails
      - Download recordings directly from web interface
      - Delete and rename recordings
  
  - **Recording Management**: Complete recording lifecycle management
    - Recording list with metadata (name, duration, size, timestamp)
    - Automatic thumbnail generation (extracted from first frame)
    - Recording metadata persistence (JSON-based)
    - Delete and rename recordings
    - Download recordings via web interface
  
  - **Configuration Persistence**: All recording settings are automatically saved
    - Video settings (resolution, FPS, codec, bitrate, presets)
    - Audio settings (codec, bitrate, include audio flag)
    - Output settings (container, output path, filename template)
    - Settings restored on application startup
  
  - **FileRecorder**: Specialized file recording implementation
    - Writes encoded data to local files
    - Supports MP4, MKV, and AVI containers
    - Automatic directory creation
    - File size and duration tracking
  
  - **RecordingMetadata**: Metadata management for recordings
    - JSON-based metadata storage
    - Thumbnail generation and storage
    - Recording information tracking

- **Raspberry Pi Support**: Native ARM builds for Raspberry Pi

  - **ARM32v7 (ARMv7)**: Support for Raspberry Pi 2, 3, and Zero
  - **ARM64v8 (ARM64)**: Support for Raspberry Pi 4 and newer
  - **Docker-based Builds**: Automated Docker builds for ARM architectures
  - **DirectFB Support**: SDL2 with DirectFB for headless operation on ARM
  - **Build Scripts**: Automated build scripts for Raspberry Pi deployment
    - `build-on-raspberry-pi.sh`: Build directly on Raspberry Pi
    - `build-linux-arm32v7-docker.sh`: Cross-compile ARM32v7 from x86_64
    - `build-linux-arm64v8-docker.sh`: Cross-compile ARM64v8 from x86_64
    - `install-deps-raspberry-pi.sh`: Install dependencies on Raspberry Pi
    - `sync-source-raspiberry.sh`: Sync source code to Raspberry Pi for building

- **Capture Presets**: Save and load capture configurations

  - **Preset System**: Save complete capture configurations
    - Device selection
    - Resolution and framerate
    - Hardware controls (V4L2/DirectShow)
    - Shader selection
    - All capture-related settings
  - **Preset Management**: Load, save, and manage presets
    - Save presets with custom names
    - Load presets to restore complete configuration
    - Preset persistence across application restarts

- **Audio Configuration Improvements**:

  - **PulseAudio Integration**: Enhanced PulseAudio support on Linux
    - Automatic cleanup of orphaned sinks and loopbacks
    - Proper sink naming and identification
    - Thread-safe mainloop access
    - Connection persistence across restarts
  - **Audio Device Selection**: Improved audio device management
    - Input source selection and persistence
    - Configuration saved and restored automatically
    - Web and local UI support for audio device selection

- **Standalone Script**: Automated audio routing script for PipeWire

  - **retrocapture-standalone.sh**: Complete standalone deployment script
    - Automatic PipeWire link creation
    - Dynamic loopback ID discovery
    - Watchdog for application monitoring
    - Automatic reconnection on restart
    - Output sink detection and fallback

### Changed

- **Architecture Refactoring**:
  - Renamed `StreamSynchronizer` to `MediaSynchronizer` for clarity
  - Shared synchronization system between streaming and recording
  - Improved code reuse between streaming and recording features
  - Better separation of concerns in encoding pipeline

- **Build System**:
  - Updated Docker base to Ubuntu 24.04 for better compatibility
  - Enhanced cross-compilation support for ARM architectures
  - Improved Docker build scripts for multiple platforms
  - Better dependency management for ARM builds

- **Configuration System**:
  - Extended configuration persistence to include recording settings
  - Added output resolution (outputWidth, outputHeight) to image settings
  - Improved configuration loading and saving reliability

- **UI Improvements**:
  - Modular recording UI components
  - Better organization of recording settings
  - Improved visual feedback for recording status
  - Enhanced web portal recording interface

### Fixed

- **Recording System**:
  - Fixed frame capture for recording with shaders applied
  - Fixed MP4 recording file truncation and moov atom issues
  - Fixed MKV recording H.264 extradata extraction and memory corruption
  - Fixed recording deletion dialog and file handling
  - Fixed thumbnail generation and deletion
  - Fixed filesystem compatibility issues with recording paths
  - Fixed frame capture logic to match streaming behavior

- **Audio System**:
  - Fixed PulseAudio mainloop thread-safety issues
  - Fixed audio sink cleanup on application close
  - Fixed audio connection persistence
  - Fixed orphaned sink and loopback cleanup

- **Web Portal**:
  - Fixed web portal navigation and cache issues
  - Fixed text visibility in recording cards
  - Fixed recording management UI responsiveness

- **Build System**:
  - Fixed Docker build compatibility issues
  - Fixed ARM build dependencies
  - Fixed filesystem compatibility in build scripts

### Technical Details

- **New Dependencies**: 
  - No new external dependencies (reuses existing FFmpeg libraries)
- **New Files**:
  - `src/recording/RecordingManager.h/cpp`: Recording orchestration
  - `src/recording/FileRecorder.h/cpp`: File recording implementation
  - `src/recording/RecordingSettings.h`: Recording configuration structure
  - `src/recording/RecordingMetadata.h/cpp`: Recording metadata management
  - `src/ui/UIConfigurationRecording.h/cpp`: Recording configuration UI
  - `src/ui/UIRecordings.h/cpp`: Recording management UI
  - `tools/retrocapture-standalone.sh`: Standalone deployment script
  - `tools/build-linux-arm32v7-docker.sh`: ARM32v7 build script
  - `tools/build-linux-arm64v8-docker.sh`: ARM64v8 build script
  - `tools/build-on-raspberry-pi.sh`: Native Raspberry Pi build script
  - `tools/install-deps-raspberry-pi.sh`: Raspberry Pi dependency installer
  - `tools/sync-source-raspiberry.sh`: Source sync script for Raspberry Pi
  - `tools/check-directfb.sh`: DirectFB compatibility checker
- **Modified Files**:
  - `src/core/Application.cpp`: Recording integration
  - `src/ui/UIManager.h/cpp`: Recording UI integration
  - `src/streaming/APIController.cpp`: Recording API endpoints
  - `src/web/control.js`: Recording web interface
  - `src/web/style.css`: Recording UI styles
  - `CMakeLists.txt`: Recording build configuration
  - `tools/build-windows-installer.sh`: Updated for new features

---

## [0.4.0-alpha] - 2025-12-15

### Added

- **Cross-Platform Support**: Native Windows support alongside existing Linux support

  - **Windows Video Capture**: DirectShow implementation (`VideoCaptureDS`)
    - Enumeration of DirectShow capture devices
    - Hardware controls (brightness, contrast, saturation, etc.)
    - Dummy mode for testing without capture device
    - Compatible with MinGW/MXE and works in Wine
  
  - **Windows Audio Capture**: WASAPI implementation (`AudioCaptureWASAPI`)
    - System audio capture via Windows Audio Session API
    - Device enumeration via MMDevice API
    - Thread-based capture for better performance
  
  - **Cross-Platform Architecture**:
    - Abstract interfaces: `IVideoCapture` and `IAudioCapture`
    - Factory pattern for platform-specific implementations
    - Automatic platform detection in build system
    - Shared code between platforms where possible

- **Windows Distribution**:
  - **Windows Installer (NSIS)**: Complete installer with all components
    - Executable with embedded icon
    - All required DLLs
    - Shaders, assets, web portal, and SSL certificates
    - Start Menu shortcuts with proper icons
  - **Application Icon**: Embedded icon in Windows executable
    - Icon appears in taskbar, file explorer, and shortcuts
    - Icon configured in installer and application shortcuts

- **Linux Improvements**:
  - **WM_CLASS Configuration**: Proper application identification in window managers
    - Application appears correctly in taskbar/launcher
    - Proper window manager integration using X11
    - Fixed "Unknown" application name issue

- **Web Portal Enhancements**:
  - **Platform Detection**: Automatic platform detection in web interface
  - **Windows API Endpoints**: DirectShow-specific API endpoints
    - `GET /api/v1/platform`: Platform and source type information
    - `GET /api/v1/ds/devices`: DirectShow device enumeration
    - `POST /api/v1/ds/device`: DirectShow device selection
  - **Adaptive UI**: Interface adapts based on detected platform

- **Build System**:
  - Cross-compilation support for Windows from Linux
  - Docker-based build system for Windows
  - MXE/MinGW-w64 support
  - Automatic platform-specific file exclusion

### Changed

- **Architecture Refactoring**:
  - Migrated to abstract interfaces for video and audio capture
  - Factory pattern for platform-specific implementations
  - Improved code organization and separation of concerns
  - Better cross-platform compatibility

- **Documentation**:
  - Updated README.md with cross-platform information
  - Updated ARCHITECTURE.md with multiplatform architecture details
  - Replaced version-specific "What's New" sections with general "Key Features"
  - Removed temporary planning documents

- **Code Standardization**:
  - Standardized code formatting (braces on new lines, spacing)
  - Translated log messages and comments from Portuguese to English
  - Consistent code style across all files

### Fixed

- **Linux Window Manager**:
  - Fixed application appearing as "Unknown" in taskbar/launcher
  - Proper WM_CLASS configuration using X11

- **Windows Networking**:
  - Fixed Winsock initialization for Windows networking
  - Proper cleanup of Winsock resources

### Technical Details

- **New Dependencies**: 
  - Windows: DirectShow (COM), WASAPI, Winsock2
  - Linux: X11 (for window manager integration)
- **New Files**:
  - `src/capture/IVideoCapture.h`: Video capture interface
  - `src/capture/VideoCaptureFactory.h/cpp`: Video capture factory
  - `src/capture/VideoCaptureDS.h/cpp`: Windows DirectShow implementation
  - `src/audio/IAudioCapture.h`: Audio capture interface
  - `src/audio/AudioCaptureFactory.h/cpp`: Audio capture factory
  - `src/audio/AudioCaptureWASAPI.h/cpp`: Windows WASAPI implementation
  - `src/retrocapture.rc`: Windows resource file for icon
  - `assets/logo.ico`: Application icon
- **Modified Files**:
  - `CMakeLists.txt`: Cross-platform build configuration
  - `src/core/Application.cpp`: Factory usage
  - `src/output/WindowManager.cpp`: WM_CLASS configuration (Linux)
  - `src/streaming/HTTPServer.cpp`: Winsock support (Windows)
  - `src/streaming/APIController.cpp`: Windows API endpoints
  - `src/web/control.js`: Platform detection
  - `README.md`: Cross-platform documentation
  - `docs/ARCHITECTURE.md`: Multiplatform architecture

---

## [0.3.0-alpha] - 2025-12-03

### Added

- **Web Portal with Remote Control**: Complete web-based control interface

  - Full REST API for remote control of all application features
  - Real-time status updates and bidirectional communication
  - Control capture sources, shaders, resolution, FPS, image adjustments, streaming settings, and V4L2 controls
  - Modern, responsive UI with RetroCapture styleguide
  - Customizable colors, images, and text labels
  - Independent operation from streaming (can be enabled/disabled separately)

- **Progressive Web App (PWA) Support**:

  - Installable on mobile devices and desktop
  - Service worker for offline functionality and caching
  - App manifest with icons and metadata
  - Works on local network IPs (requires HTTPS)

- **HTTPS/SSL Support**:

  - Optional HTTPS for web portal and streaming
  - Self-signed certificate support for local development
  - Configurable SSL certificate and key paths
  - Support for Subject Alternative Names (SANs) for local network IPs

- **Source Type Selection**:

  - New `--source` parameter to select source type (none, v4l2)
  - Dummy mode for testing without capture device
  - V4L2 controls only applied when source is V4L2

- **Enhanced CLI Parameters**:

  - `--source <type>`: Select source type (none, v4l2)
  - `--v4l2-device <path>`: V4L2 device path (renamed from --device for consistency)
  - `--web-portal-enable/--web-portal-disable`: Enable/disable web portal
  - `--web-portal-port <port>`: Web portal port
  - `--web-portal-https`: Enable HTTPS
  - `--web-portal-ssl-cert <path>`: SSL certificate path
  - `--web-portal-ssl-key <path>`: SSL key path

- **REST API Endpoints**:

  - `GET /api/source`: Get current source configuration
  - `POST /api/source`: Set source type and device
  - `GET /api/shader`: Get current shader
  - `POST /api/shader`: Set shader
  - `GET /api/shader/list`: Get available shaders
  - `GET /api/shader/parameters`: Get shader parameters
  - `POST /api/shader/parameter`: Set shader parameter
  - `GET /api/capture/resolution`: Get capture resolution
  - `POST /api/capture/resolution`: Set capture resolution
  - `GET /api/capture/fps`: Get capture FPS
  - `POST /api/capture/fps`: Set capture FPS
  - `GET /api/image`: Get image settings
  - `POST /api/image`: Set image settings
  - `GET /api/streaming/settings`: Get streaming settings
  - `POST /api/streaming/settings`: Set streaming settings
  - `POST /api/streaming/control`: Start/stop streaming
  - `GET /api/streaming/status`: Get streaming status
  - `GET /api/v4l2/devices`: Get available V4L2 devices
  - `POST /api/v4l2/devices/refresh`: Refresh V4L2 device list
  - `GET /api/v4l2/controls`: Get V4L2 controls
  - `POST /api/v4l2/control`: Set V4L2 control value
  - `POST /api/v4l2/device`: Set V4L2 device

- **Streaming Cooldown System**:

  - 10-second cooldown after stopping streaming
  - Prevents rapid start/stop cycles that could cause issues
  - Visual feedback in both local and web UIs
  - API returns cooldown status and remaining time

- **UI Refactoring**:

  - Modular UI architecture with separate classes per window/tab
  - `UIConfiguration`: Main configuration window
  - `UIConfigurationSource`: Source selection and V4L2 controls
  - `UIConfigurationImage`: Image adjustments
  - `UIConfigurationShader`: Shader selection and parameters
  - `UIConfigurationStreaming`: Streaming configuration
  - `UIConfigurationWebPortal`: Web portal settings
  - `UIInfoPanel`: Information display
  - Better code organization and maintainability

- **Real-time Updates**:
  - All controls update in real-time without apply/save buttons
  - Immediate feedback for all parameter changes
  - Synchronized state between local UI and web portal

### Changed

- **CLI Parameter Reorganization**:

  - `--device` renamed to `--v4l2-device` for consistency
  - V4L2 parameters grouped together in help
  - V4L2 parameters only applied when `--source v4l2`
  - Improved parameter validation and error messages

- **Web Portal Architecture**:

  - Web portal can run independently of streaming
  - Same HTTP server used for both portal and streaming
  - Improved request routing and static file serving
  - Better error handling and 404 responses

- **Streaming Architecture**:

  - Simplified resource management
  - Better thread synchronization
  - Improved cleanup and shutdown procedures

- **Audio Processing**:
  - Continuous PulseAudio mainloop processing even when streaming is inactive
  - Prevents system audio freezing
  - Multiple mainloop iterations per call for better responsiveness

### Fixed

- **Audio System**:

  - Fixed critical issue where RetroCapture would freeze entire PC audio after extended use
  - Audio now processes correctly even when streaming is not active
  - Proper PulseAudio mainloop handling

- **Streaming Stability**:

  - Fixed heap corruption issues during streaming stop/restart
  - Improved resource cleanup and deallocation
  - Better error handling and recovery

- **Web Portal**:

  - Fixed V4L2 device list not populating correctly
  - Fixed CSS issues with sliders and controls
  - Fixed infinite recursion in slider controls
  - Fixed streaming status not updating in frontend
  - Fixed button state management during start/stop operations

- **UI**:
  - Fixed button being clickable multiple times during stop operation
  - Fixed processing state not being properly displayed
  - Fixed cooldown display in both local and web UIs

### Removed

- **Unused Code**:
  - Removed `HTTPMJPEGStreamer` class (not being used)
  - Removed HLS/player.js related code (file doesn't exist)
  - Removed `setHLSParameters()` method and related variables
  - Removed `libmicrohttpd` dependency from CMakeLists.txt
  - Removed `--stream-quality` CLI parameter (was for MJPEG)
  - Removed `--stream-audio-buffer-size` CLI parameter (managed automatically)
  - Removed `setStreamingQuality()` method and `m_streamingQuality` variable

### Technical Details

- **New Dependencies**: OpenSSL (optional, for HTTPS support)
- **New Files**:
  - `src/streaming/WebPortal.h/cpp`: Web portal implementation
  - `src/streaming/HTTPServer.h/cpp`: HTTP/HTTPS server implementation
  - `src/streaming/APIController.h/cpp`: REST API controller
  - `src/ui/UIConfiguration*.h/cpp`: Modular UI components
  - `src/web/`: Web portal static files (HTML, CSS, JS)
  - `src/web/manifest.json`: PWA manifest
  - `src/web/service-worker.js`: Service worker for PWA
- **Statistics**: 29 commits, significant refactoring and new features

---

## [0.2.0-alpha] - 2025-11-22

### Added

- **HTTP MPEG-TS Streaming**: Complete streaming system with audio and video support

  - HTTP server for client connections
  - MPEG-TS container format
  - Real-time encoding and muxing
  - Multiple concurrent client support
  - Stream URL display in UI

- **Multi-Codec Support**:

  - **H.264 (libx264)**: Full support with quality presets (ultrafast to veryslow)
  - **H.265/HEVC (libx265)**: Support with presets, profiles (main, main10), and levels (1.0 to 6.2)
  - **VP8 (libvpx-vp8)**: Support with speed control (0-16)
  - **VP9 (libvpx-vp9)**: Support with speed control (0-9)
  - Codec selection via UI dropdown
  - Codec-specific configuration options

- **Audio Capture and Streaming**:

  - PulseAudio integration for system audio capture
  - Virtual sink creation for audio routing
  - AAC audio encoding
  - Real-time audio/video synchronization
  - Configurable sample rate and channel count

- **Streaming Architecture**:

  - `StreamManager`: Orchestrates multiple streaming implementations
  - `IStreamer`: Abstract interface for extensible streaming protocols
  - `HTTPTSStreamer`: HTTP MPEG-TS implementation
  - Multi-threaded encoding pipeline (separate thread for encoding)
  - Timestamp-based synchronization system
  - Buffer management with overflow protection

- **UI Enhancements**:

  - New "Stream" tab in settings interface
  - Codec selection dropdown
  - Resolution and FPS dropdowns (replacing text inputs)
  - H.264 quality preset dropdown
  - H.265 preset, profile, and level dropdowns
  - VP8/VP9 speed sliders
  - Video and audio bitrate configuration
  - Stream status display (active clients, stream URL)
  - Start/stop streaming controls

- **Configuration Persistence**:

  - Automatic saving of all settings to `config.json`
  - Automatic loading of settings on application startup
  - Includes streaming configuration, shader parameters, and UI preferences
  - Uses `nlohmann/json` for JSON serialization

- **Synchronization System**:

  - Timestamp-based audio/video synchronization using `CLOCK_MONOTONIC`
  - Master clock synchronization (audio as master)
  - Sync zone calculation for frame pairing
  - Desynchronization detection and automatic recovery
  - Frame skipping for buffer management

- **Build System Improvements**:

  - Automatic copying of `shaders/` directory to build output
  - CMake POST_BUILD custom command for shader deployment
  - Ensures shaders are found at runtime

- **Code Quality**:
  - All compilation warnings resolved
  - Deprecated API usage replaced (`AVFrame::key_frame` → `AV_FRAME_FLAG_KEY`)
  - Unused variables and functions removed
  - Improved code organization and structure

### Changed

- **Architecture Refactoring**:

  - Migrated to smart pointers (`std::unique_ptr`) throughout codebase
  - Applied SOLID principles with extracted utility classes
  - Improved separation of concerns
  - Better memory management and resource cleanup

- **Threading Model**:

  - Changed from single-threaded to hybrid multi-threaded architecture
  - Main thread: capture, processing, rendering, UI
  - Encoding thread: video/audio encoding and muxing
  - Server thread: HTTP client connections
  - Client threads: per-client packet forwarding

- **UI Improvements**:

  - Menu fixed at top, configuration window floating
  - Better organization of streaming settings
  - Improved dropdown interfaces for common settings
  - Renamed `imgui.ini` to `RetroCapture.ini`

- **Streaming Pipeline**:
  - Frame capture moved to main thread with timestamped buffers
  - Encoding happens in dedicated thread
  - Improved frame processing and queue management
  - Better handling of frame drops and buffer overflow

### Fixed

- **Streaming Stability**:

  - Fixed packet corruption issues in MPEG-TS stream
  - Fixed "Invalid NAL unit 0" errors
  - Fixed "unspecified size" errors for video stream
  - Fixed DTS monotonicity violations
  - Fixed audio/video desynchronization
  - Fixed codec packet generation issues (x264 lookahead threads)
  - Fixed `SwsContext` initialization with invalid dimensions (0x1056)
  - Fixed graceful shutdown and cleanup (prevented crashes on stop/restart)
  - Fixed "Address already in use" socket errors
  - Fixed memory corruption and double-free errors
  - Fixed application freezing during streaming

- **UI and Configuration**:

  - Fixed hardcoded FPS values in buffer thresholds
  - Fixed UI value interpretation (dropdown vs text input)
  - Fixed window minimization in fullscreen mode
  - Fixed application pause when losing focus
  - Fixed streaming crash on window resize

- **Codec Issues**:

  - Fixed VP8/VP9 stream recognition (extradata generation)
  - Fixed codec parameter updates
  - Fixed keyframe generation and periodic I-frames
  - Fixed codec state management and flushing

- **Build and Compilation**:
  - Fixed all compiler warnings
  - Fixed deprecated API usage
  - Fixed unused parameter warnings
  - Fixed control flow warnings

### Performance

- Optimized encoding thread to reduce lock contention
- Implemented frame queue with overflow protection
- Optimized FPS with libswscale improvements
- Reduced frame loss with better buffer management
- Optimized audio and video processing pipeline
- Improved frame skipping for buffer management
- Reduced latency with low-latency codec presets

### Technical Details

- **New Dependencies**: FFmpeg (libavcodec, libavformat, libavutil, libswscale, libswresample), PulseAudio, nlohmann/json
- **New Files**:
  - `src/streaming/` directory with streaming implementation
  - `src/audio/AudioCapture.h/cpp` for audio capture
  - `src/utils/ShaderScanner.h/cpp`, `V4L2DeviceScanner.h/cpp` (extracted utilities)
  - `src/v4l2/V4L2ControlMapper.h/cpp` (extracted utility)
  - `src/shader/ShaderPreprocessor.h/cpp` (extracted from ShaderEngine)
- **Statistics**: 42 commits, ~7,578 lines added, ~1,514 lines removed

### Known Issues

- VP8/VP9 streams may appear as "Data: bin_data" in some players (implementation incomplete)
- Audio/video synchronization may drift under heavy system load
- `glReadPixels` is synchronous and may block main thread at high resolutions
- HTTP streaming has no authentication (stream is publicly accessible)
- Some codec-specific advanced settings not yet exposed in UI

---

## [0.1.0-alpha] - 2025-11-13

### Added

- Initial release
- Real-time V4L2 video capture
- OpenGL 3.3+ rendering with GLFW
- YUYV to RGB conversion
- Logging system
- Low-latency pipeline
- Memory mapping for V4L2 buffers
- Non-blocking capture
- Basic shader support
- Modular architecture (capture, renderer, shader engine)
- Support for RetroArch GLSL shaders and presets (`.glslp`)
- Multi-pass shader preset support
- Graphical user interface with ImGui
- V4L2 hardware controls (brightness, contrast, saturation, hue, gain, exposure, sharpness, gamma, white balance)
- Fullscreen and multi-monitor support
- Aspect ratio maintenance (letterboxing/pillarboxing)
- Real-time shader parameter editing
- Shader preset saving (overwrite and save as)
- V4L2 device selection via GUI
- Dynamic resolution and framerate configuration
- Command-line interface with comprehensive parameter support
- Support for RetroArch uniforms: `OutputSize`, `InputSize`, `SourceSize`, `FrameCount`, `TextureSize`, `MVPMatrix`, `FrameDirection`
- Reference texture loading (LUTs, PNG images)
- Shader parameter extraction from `#pragma parameter` directives
- Automatic shader type detection and correction
- History buffer support for animated shaders

### Changed

- Improved shader path resolution for RetroArch-style relative paths
- Enhanced uniform type detection and injection
- Better error handling and logging

### Fixed

- Image orientation issues
- Shader compilation errors with type mismatches
- Framebuffer management for multi-pass shaders
- Texture binding for reference textures
- Uniform type detection (`vec2`, `vec3`, `vec4` for `OutputSize`)
- V4L2 control value validation and alignment
