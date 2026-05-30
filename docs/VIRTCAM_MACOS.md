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

## Status

| Step | Status |
|------|--------|
| Design doc (this file) | done |
| `VirtualCameraOutputMac.{h,cpp}` host sink | done |
| `src/dal_plugin/` plug-in + SharedMemoryReader | done |
| CMake bundle target + option | done |
| `.app` packaging + install helper | done |
| Application + UI wiring | done |
| Builds + installs on macOS (Intel) | done |
| **End-to-end verify (OBS sees camera, frames flow)** | **needs confirming on real consumers** |
| `CMIOObjectCreate` Device/Stream id minting | revisit — see TODO in `Plugin.mm::InitializeWithObjectID` |
| YUYV second advertised format | follow-up |
| Camera Extension (sandboxed-consumer support) | deferred (needs Developer cert) |

## References (for whoever picks up the CMIO internals)

- John Boiles' `obs-mac-virtualcam` (legacy DAL implementation — the
  canonical reference for the callback table + queue plumbing)
- Apple `CoreMediaIO/CMIOHardwarePlugIn.h` (the DAL interface)
- Apple "Creating a camera extension with Core Media I/O" (the
  Camera Extension successor path)
