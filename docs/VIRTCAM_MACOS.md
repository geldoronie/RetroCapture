# Virtual Camera on macOS — design + status (#85 macOS)

Companion to `docs/VIRTCAM_WINDOWS.md`. Same two-process architecture
(host writes frames to shared memory, a per-consumer plug-in reads
them and presents a webcam) but built on CoreMediaIO instead of
DirectShow.

The host-side IPC is implemented in
`src/output/VirtualCameraOutputMac.{h,cpp}`; the consumer-side
plug-in lives in `src/dal_plugin/` and ships as
`RetroCaptureVCam.plugin`.

## Architecture choice: DAL plug-in (not Camera Extension)

macOS has two ways to publish a virtual camera:

| | DAL plug-in (what we ship) | Camera Extension (System Extension) |
|---|---|---|
| Min OS | any modern macOS | macOS 12.3+ |
| Signing | **none required** | Apple Developer ID + entitlement + notarization |
| Sandboxed consumers (Safari, new Apple apps) | ❌ no | ✅ yes |
| OBS / Discord / Zoom / Chrome (legacy CMIO) | ✅ yes | ✅ yes |
| Apple status | deprecated since 12.3 (still loads) | the future |

We ship the **DAL plug-in** because it needs no Apple Developer
cert — same ship-first reasoning as the unsigned Windows DLL. The
Camera Extension is a deferred follow-up (it requires paid
Developer membership + notarization the project doesn't have yet).
When that lands it goes in a sibling `src/camera_extension/` target
built into the `.app`'s `Contents/Library/SystemExtensions/`; the
DAL plug-in stays as the no-cert fallback.

## Two processes, two binaries

```
+---------------------------+              +-------------------------------+
| RetroCapture (host)       |              | RetroCaptureVCam.plugin       |
| (UI / capture / shader)   |              | (loaded by CoreMediaIO into   |
|                           |              |  every consumer process)      |
|  VirtualCameraOutputMac   |   POSIX shm  |  Plugin.mm (CMIO callbacks)   |
|    start() shm_open+mmap  |  <-------->  |   frame thread:               |
|    pushFrame() sws+sem_post|   + named   |     SharedMemoryReader        |
|    stop()                 |   semaphore  |     -> CVPixelBuffer          |
|                           |              |     -> CMSampleBuffer         |
|                           |              |     -> CMSimpleQueue          |
+---------------------------+              +-------------------------------+
```

## IPC layer

Identical wire format to Windows — the structs + slot geometry live
in the shared `src/output/VirtcamIpcLayout.h`, so the only
platform-specific bits are the named-object primitives:

| | Windows | macOS |
|---|---|---|
| Shared region | `CreateFileMappingW` (`Local\RetroCaptureVCam_FrameMap_v1`) | `shm_open` (`/RCVcamShm_v1`) |
| Frame-ready signal | auto-reset event (`...FrameReady_v1`) | POSIX semaphore (`/RCVcamEvt_v1`) |
| Wait | `WaitForSingleObject` | `sem_trywait` poll @ 5 ms (macOS lacks `sem_timedwait`) |

`SharedHeader` / `FrameHeader` / the two-slot ping-pong + the
release-store-on-`writeSlot` synchronisation are byte-for-byte the
same as the Windows path. See `VirtcamIpcLayout.h` for the layout.

## CMIO object model

```
kCMIOObjectSystemObject
  └── PlugIn        (the binary, one — id from InitializeWithObjectID)
        └── Device        (the virtual camera, one)
              └── Stream  (the output stream, one)
```

`Plugin.mm` is a single TU holding the whole
`CMIOHardwarePlugInInterface` callback table + a global
`PluginState`. The frame thread (spun up by `DeviceStartStream`,
torn down by `DeviceStopStream`) loops:
`reader.waitFrame()` → `reader.snapshotFrame()` →
`CVPixelBufferCreateWithBytes` → `CMIOSampleBufferCreateForImageBuffer`
→ `CMSimpleQueueEnqueue` → notify the consumer's `queueAlteredProc`.

Fixed advertised format for the first cut: **RGB24 @ 1280×720 @
30 fps** (`kCVPixelFormatType_24RGB`). The host resizes whatever it
has into this geometry; a writer pushing a different format/size
currently falls through to "no new frame" on the consumer.

## Install

The plug-in must live at
`/Library/CoreMediaIO/Plug-Ins/DAL/RetroCaptureVCam.plugin`, which
is root-owned. The `.app` bundle ships the built `.plugin` plus a
helper under `Contents/Resources/`:

```
sudo /Applications/RetroCapture.app/Contents/Resources/install-virtcam.sh
```

(`tools/install-virtcam-macos.sh` — copy → chown root:wheel →
atomic mv, so a half-copied bundle never gets loaded.) Already-running
consumers must quit + relaunch to re-scan the DAL directory.

`VirtualCameraOutputMac::isPluginInstalled()` drives the UI driver-
status line (Configurations → Virtual Camera).

## Status — PAUSED (enumerates, won't activate on Ventura)

| Step | Status |
|------|--------|
| Design doc (this file) | done |
| `VirtualCameraOutputMac.{h,cpp}` host sink | done |
| `src/dal_plugin/` plug-in + SharedMemoryReader | done |
| CMake bundle target + option | done |
| `.app` packaging + install helper | done |
| Application + UI wiring | done |
| Builds + installs on macOS (Intel) | done |
| Device **enumerates** + appears in consumers w/ format | **done** |
| Stream **activates** + frames flow | **BLOCKED on macOS 13 (Ventura)** |
| Camera Extension (the path that actually works on 12.3+) | deferred (needs Developer cert) |

### What works

The full CMIO DAL object lifecycle is implemented and correct enough
to enumerate: `CMIOObjectCreate` for device + stream,
`CMIOStreamClockCreate`, `CMIOObjectsPublishedAndDied`, and a property
table that answers **everything** a consumer queries (verified via the
debug trace — zero `MISS` lines during a full OBS interrogation).
The device shows up in OBS with the advertised format.

### Where it's blocked

The consumer (OBS, via AVFoundation) reads the entire device + stream
property set successfully and then **declines to start the stream** —
`DeviceStartStream` is never called, so no frames ever flow. This is
consistent with macOS 12.3+ deprecating DAL: the system camera path
has moved to **CMIOExtension** (Camera Extensions), and DAL device
*activation* via AVFoundation is unreliable/restricted on Ventura even
when *enumeration* still works. The system log shows the camera
subsystem using `CMIOExtensionProvider`/`CMIOExtensionStream`.

Bring-up findings worth keeping (each was a real round-trip):

- **TransportType ('tran')** is queried during the device publish and
  is *fatal* if missing — the publish returns `'who?'` and the device
  never appears. Must return something (we return 'virt').
- **CanBeDefaultDevice ('dflt') / CanBeDefaultSystemDevice ('sflt')**
  must be answered (return 1) or the consumer treats the device as
  unusable.
- **SetPropertyData for unknown selectors must return `noErr`**, not
  an error. The consumer hammers `SetPropertyData('lisa')` (listener
  bookkeeping) during init; rejecting it put the consumer in a retry
  loop that never reached activation.
- **Pixel format**: 24RGB → device shown but no image (poorly
  supported); 32BGRA → recognised but didn't help activation;
  '2vuy'/UYVY (camera-native) is what's wired now. Format was NOT the
  activation blocker (the consumer reads the format fine and still
  doesn't start).
- The full property selector set the consumer queries (all answered):
  `uid `, `muid`, `tran`, `sdir`, `dflt`, `oink` (hog), `gone`
  (running-somewhere), `livn` (alive), `stm#` (streams), `lnam`,
  `lmak`, `pfta`/`pft ` (format descs), `frrg`/`nfrt`/`mfrt` (rates).

### To resume

1. Re-enable the trace: build the plug-in with
   `-DRETROCAPTURE_VCAM_DEBUG_LOG=1` (the `vclog` scaffolding in
   `Plugin.mm` is gated off by default now) and/or
   `log stream --predicate 'subsystem == "com.apple.cmio"'` to capture
   the *framework-level* reason AVFoundation won't activate the
   stream — that reason is invisible to the in-plug-in log and is the
   missing piece.
2. If it's the DAL-deprecation wall (likely on 13+), the real fix is
   to implement the **Camera Extension** variant (CMIOExtension):
   a System Extension in the `.app`'s
   `Contents/Library/SystemExtensions/`, activated via
   `OSSystemExtensionRequest`. Requires an Apple Developer ID, the
   `com.apple.developer.system-extension.install` entitlement, and
   notarization for distribution. Works in OBS, Discord, Safari,
   FaceTime — everything, including sandboxed/hardened consumers
   (which DAL never will).

## References (for whoever picks up the CMIO internals)

- John Boiles' `obs-mac-virtualcam` (legacy DAL implementation — the
  canonical reference for the callback table + queue plumbing)
- Apple `CoreMediaIO/CMIOHardwarePlugIn.h` (the DAL interface)
- Apple "Creating a camera extension with Core Media I/O" (the
  Camera Extension successor path)
