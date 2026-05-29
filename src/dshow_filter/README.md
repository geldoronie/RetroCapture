# RetroCaptureVCam.dll — DirectShow source filter (#85 Phase 2)

This directory holds the DirectShow source filter that exposes the
host-side shared memory ring (written by `VirtualCameraOutputWin`)
as a Windows webcam device. Consumers (OBS, Chrome, Discord, Zoom,
…) load this DLL via COM and read frames through the standard
`CSourceStream::FillBuffer` flow.

**Status:** skeleton only — header files + commented entry points.
The actual implementation is gated behind the CMake option
`RETROCAPTURE_BUILD_VIRTCAM_DSHOW_FILTER` (default OFF) because it
depends on DirectShow BaseClasses (`strmbase`) which is not present
in our MXE / MinGW cross-compile toolchain.

To complete Phase 2:

1. **Get strmbase building.** Two practical paths:
   - On a Windows dev box: use the BaseClasses copy that ships
     with the Windows SDK Samples
     (`<Windows Kits>\10\Samples\multimedia\directshow\baseclasses\`).
   - Cross-compile: vendor a clean BaseClasses copy into
     `third_party/strmbase/` and add a CMake target for it.
     The Wine project's strmbase port + the ReactOS port are
     both viable starting points.

2. **Implement** the classes laid out in `docs/VIRTCAM_WINDOWS.md`:
   `CRetroCaptureFilter` (CSource subclass), `CRetroCaptureStream`
   (CSourceStream subclass), `SharedMemoryReader` (OpenFileMappingW
   + WaitForSingleObject on `Local\RetroCaptureVCam_FrameReady_v1`).

3. **Wire registry registration** in `DllSetup` — `AMOVIESETUP_*`
   tables consumed by the `DllRegisterServer` helper in BaseClasses.

4. **Add NSIS post-install** `regsvr32 /s RetroCaptureVCam.dll`.

5. **End-to-end test on Windows**: install, RetroCapture writes,
   OBS reads, picture appears.

See `docs/VIRTCAM_WINDOWS.md` for the full architecture, IPC
protocol (shared memory layout, event names, CLSIDs), and CMake
+ CPack hooks.
