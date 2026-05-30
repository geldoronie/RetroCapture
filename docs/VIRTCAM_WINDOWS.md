# Virtual Camera on Windows — design doc (#85 Phase 2)

This document specifies the Windows DirectShow virtual camera so any
developer with Windows + Visual Studio (or the right MinGW setup) can
pick it up and implement the kernel-mode DLL without re-deriving the
architecture.

The host-side IPC is implemented in `src/output/VirtualCameraOutputWin.{h,cpp}`
and cross-compiles cleanly today. The DLL side is stubbed out under
`src/dshow_filter/` and gated by a CMake option (off by default)
because it depends on **DirectShow BaseClasses** (`strmbase`), which
are not present in the MXE / MinGW toolchain we use for CI.

## Two processes, two binaries

```
+---------------------------+              +-------------------------------+
| RetroCapture.exe          |              | RetroCaptureVCam.dll          |
| (UI / capture / shader)   |              | (loaded inside every consumer |
|                           |              |  process — OBS.exe, chrome,   |
|                           |              |  zoom.exe, …)                 |
|                           |              |                               |
|  +--------------------+   |              |   +-----------------------+   |
|  | VirtualCamera-     |   |              |   | CRetroCaptureFilter   |   |
|  | OutputWin          |   |              |   | (CSource subclass)    |   |
|  |   open()           |   |              |   |   FillBuffer()        |   |
|  |   pushFrame()      |   |              |   +-----------+-----------+   |
|  |   close()          |   |              |               |               |
|  +---------+----------+   |              |               v               |
|            |              |              |   reads latest frame from     |
|            v              |              |   shared memory               |
|   writes RGBA/YUYV to     |   IPC        |   blocks (with timeout) on    |
|   shared memory ring;     |  <-------->  |   the "frame ready" event     |
|   signals "frame ready"   |              |                               |
|   event                   |              |                               |
+---------------------------+              +-------------------------------+
```

**Why a separate DLL**: DirectShow source filters live inside the
consumer process. OBS loads the filter DLL, instantiates the COM
object, and calls `FillBuffer` to pull frames. The DLL therefore
cannot directly call into RetroCapture — they're different processes.
Win32 named shared memory + named events bridge them.

## CLSID + registry

Fixed per build. Picked once, never changes (multiple RetroCapture
installs on the same machine share the same virtual camera device by
design):

```
CLSID:        {C4F2E1A0-7B3D-4F8E-9C1B-RC850000VCAM}      (12 hex chars after RC85 to namespace)
Filter name:  "RetroCapture Virtual Camera"
Category:     CLSID_VideoInputDeviceCategory
              ({860BB310-5D01-11d0-BD3B-00A0C911CE86})
```

Registered under:

```
HKCR\CLSID\{...vcam-clsid...}\InprocServer32 = "C:\Program Files\RetroCapture\RetroCaptureVCam.dll"
HKCR\CLSID\{...vcam-clsid...}\InprocServer32\ThreadingModel = "Both"
HKCR\CLSID\{...category-clsid...}\Instance\{...vcam-clsid...}\FriendlyName = "RetroCapture Virtual Camera"
HKCR\CLSID\{...category-clsid...}\Instance\{...vcam-clsid...}\CLSID = "{...vcam-clsid...}"
```

`DllRegisterServer` + `DllUnregisterServer` (exported from the DLL)
write / remove these keys. The installer calls `regsvr32 /s
RetroCaptureVCam.dll` post-install and `regsvr32 /u /s ...` on
uninstall.

## IPC layer (shared memory + events)

### Named objects

Both objects live in the `Local\` namespace (per-session) so two
users on the same Windows host don't clash:

| Object        | Kind            | Name                                    | Lifetime              |
|---------------|-----------------|-----------------------------------------|------------------------|
| Frame map     | File mapping    | `Local\RetroCaptureVCam_FrameMap_v1`    | Created by RetroCapture; consumers MapViewOfFile to read |
| Frame ready   | Auto-reset event| `Local\RetroCaptureVCam_FrameReady_v1`  | Signalled by RetroCapture after each frame write |
| Frame mutex   | Mutex           | `Local\RetroCaptureVCam_FrameLock_v1`   | Optional, gates the shared memory region against torn reads. v1 uses double-buffering instead (see below) so the mutex is currently unused. |

The `_v1` suffix is a forward compatibility hook: a future protocol
rev bumps to `_v2` so old + new RetroCapture instances can coexist
without colliding on the same names.

### Shared memory layout

The file mapping is a fixed 64 MB region (large enough for 4K RGBA
+ headroom). Two ping-pong slots so a reader picking up frame N while
the writer is laying down N+1 doesn't tear:

```
offset   size      field
-------------------------------------------------------------
0        4         uint32 magic = 'RCVC' (0x52435643)
4        4         uint32 version = 1
8        4         uint32 writeSlot     — which slot the writer last completed (0 or 1)
12       4         uint32 reserved
16       64        char[64] cardLabel   — "RetroCapture", UTF-8, null-terminated
80       16        reserved
96       ...       slot[0] header + payload
slot1Off ...       slot[1] header + payload
```

Slot offsets: slot0 starts at 96, slot1 starts at `96 + slotSize`,
`slotSize = sizeof(FrameHeader) + maxBytes`. Hard-coded
`maxBytes = 3840*2160*4 = 33,177,600` for 4K RGBA so the geometry
is fixed and consumers can address slots without computation.

```c
struct FrameHeader {
    uint32_t magic;        // 'RCFR' (0x52434652) — also a torn-write guard
    uint32_t width;
    uint32_t height;
    uint32_t pixelFormat;  // 1 = RGBA, 2 = RGB24, 3 = YUYV — wire enum, not V4L2 fourcc
    uint64_t timestamp100ns;// QueryPerformanceCounter-derived, host monotonic
    uint32_t payloadBytes; // width*height*bpp
    uint32_t reserved;
};
```

### Write flow (RetroCapture)

```
1. nextSlot = (currentWriteSlot + 1) & 1
2. write FrameHeader + payload into slot[nextSlot]
3. memory barrier (atomic store with release semantics)
4. update header.writeSlot = nextSlot   (single atomic uint32)
5. SetEvent(frameReadyEvent)            (auto-reset)
```

The slot flip happens AFTER the data is written, so a reader that
sees the new `writeSlot` is guaranteed to see complete data.

### Read flow (DirectShow filter)

```
1. WaitForSingleObject(frameReadyEvent, timeout)
2. read header.writeSlot atomically
3. memcpy slot[writeSlot] into the DirectShow IMediaSample buffer
4. set IMediaSample timestamps (use FrameHeader.timestamp100ns or stream clock)
5. return S_OK
```

When the event isn't signalled within the timeout, return the previously
delivered frame (or a frozen-frame copy maintained inside the filter)
to keep the camera "alive" from the consumer's perspective. Don't
return `S_FALSE` — most consumers treat that as end-of-stream.

## DLL file structure

```
src/dshow_filter/
    DllEntry.cpp              DllMain, DllGetClassObject, DllRegisterServer, DllUnregisterServer
    DllSetup.{h,cpp}          AMOVIESETUP_* tables, registry helpers
    RetroCaptureFilter.{h,cpp}  CSource subclass — exposes the pin set
    RetroCaptureStream.{h,cpp}  CSourceStream subclass — FillBuffer, GetMediaType, etc.
    SharedMemoryReader.{h,cpp}  Wraps OpenFileMapping + WaitForSingleObject(frameReadyEvent)
    RetroCaptureFilter.def    LIBRARY exports for the COM entry points
    CLSIDs.h                  GUIDs (one for the filter, one for the property page if any)
```

### Filter responsibilities

1. **Media-type negotiation**: advertise RGB24 + YUYV at a small list
   of common resolutions (640×480, 1280×720, 1920×1080). On
   `SetFormat`, lock in whatever the consumer picked and tell
   `SharedMemoryReader` to expect that geometry. Convert on read if
   the host writes a different format than the consumer picked.
2. **Stream timing**: pace with `IReferenceClock`. Most consumers
   set their own clock; the filter just respects the requested
   frame rate and returns the latest frame at each tick.
3. **Idle handling**: when RetroCapture isn't writing (toggle off,
   process not running), the event never fires. Return a frozen
   frame (black / "RetroCapture not connected" generated overlay)
   so consumers don't hang.

## Host-side sink (this PR)

`src/output/VirtualCameraOutputWin.{h,cpp}` mirrors the Linux
sink's interface so `Application::syncVirtualCamera` can be
made cross-platform with a typedef switch. The file:

- `open(width, height, pixelFormat, error)` — `CreateFileMappingW` +
  `CreateEventW`, format the header, lay out the slots.
- `pushFrame(pixels, srcWidth, srcHeight, srcFormat)` — convert
  via libswscale to the negotiated format (RGB24 / YUYV — same code
  the Linux sink already runs), write to the next slot, atomic
  flip `writeSlot`, `SetEvent`.
- `close()` — `CloseHandle` on event + map.

No DirectShow dependency. Compiles with vanilla MinGW + the standard
Win32 SDK headers (`<windows.h>`).

## Build + distribution

### CMake

A new option `RETROCAPTURE_BUILD_VIRTCAM_DSHOW_FILTER` (default
OFF) gates the DLL target. When ON, CMake builds the DLL from
`src/dshow_filter/` and links against `strmbase` (which must be
available on the system — for MinGW, set
`STRMBASE_ROOT` to a vendored / built copy; for MSVC, it's in
the Windows SDK Samples). The host build doesn't need the option.

### CPack / NSIS

The NSIS installer config gains:

```nsis
Section "Install"
  ...
  File "RetroCaptureVCam.dll"
  ExecWait 'regsvr32.exe /s "$INSTDIR\RetroCaptureVCam.dll"'
  ...
SectionEnd

Section "Uninstall"
  ExecWait 'regsvr32.exe /u /s "$INSTDIR\RetroCaptureVCam.dll"'
  Delete "$INSTDIR\RetroCaptureVCam.dll"
  ...
SectionEnd
```

The installer already requests admin via NSIS `RequestExecutionLevel
admin`, so the regsvr32 call runs in the elevated context.

### Smart Screen / Defender

Without an Authenticode signature, SmartScreen will pop a "Windows
protected your PC" warning on first run of the installer. User
clicks "More info → Run anyway". Document this in the README's
Windows install section.

Signing the DLL (and the host installer) with an Authenticode
cert removes the warning. Defer until the cert is available;
unsigned builds work, just with the warning.

## Phase-2 roadmap

| Step | Owner / where | Status |
|------|---------------|--------|
| Design doc (this file)                       | host-side commit | done |
| `VirtualCameraOutputWin.{h,cpp}`             | host-side commit | done |
| CMake option + `src/dshow_filter/` skeleton  | host-side commit | done |
| Vendor or wire strmbase                      | Windows dev      | open |
| `CRetroCaptureFilter` + `CRetroCaptureStream`| Windows dev      | open |
| `SharedMemoryReader` (filter-side)           | Windows dev      | open |
| `DllRegisterServer` registry writes          | Windows dev      | open |
| NSIS post-install regsvr32                   | host packaging   | open |
| End-to-end test on Windows real              | manual           | open |

## References (for the Windows dev picking this up)

- DirectShow BaseClasses overview — `<Windows SDK>/Samples/multimedia/directshow/baseclasses/`
- OBS-VirtualCam (now obsolete but its design is the canonical inspiration):
  legacy GitHub: `CatxFish/obs-virtual-cam`
- Microsoft "Writing Source Filters" — search the DirectShow docs
- "Virtual Webcam" sample in the Windows SDK Samples (DirectShow)
