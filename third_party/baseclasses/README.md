# DirectShow BaseClasses (vendored)

Microsoft's DirectShow BaseClasses (the C++ helper library — `CSource`,
`CSourceStream`, `CBaseFilter`, ...) are required to implement
`RetroCaptureVCam.dll` (the Windows virtual camera filter, #85 Phase 2)
but are not shipped with MinGW or MXE. This directory holds a
vendored snapshot patched for MinGW/Clang compatibility, sourced
from:

- Upstream: https://github.com/TianZerL/DirectShow-BaseClasses-MultiCompiler
- Commit:   `8285459` ("Fix bug.")
- License:  MIT (see `LICENSE`). The underlying Microsoft source is
            MS-PL; the MinGW/Clang patches added by TianZer are MIT.

We don't use TianZer's `CMakeLists.txt` because it requires
CMake 3.15 and our cross-build toolchain (dockcross / MXE) ships
CMake 3.10. The build wiring lives in `CMakeLists.txt` next to this
file and produces the `strmbase` static library when the option
`RETROCAPTURE_BUILD_VIRTCAM_DSHOW_FILTER` is ON.

## Local patches

Two surgical changes on top of the upstream snapshot:

1. **Case-sensitive include** — `refclock.h` references
   `#include "Schedule.h"` (capital S); the file is `schedule.h`.
   Windows/macOS filesystems are case-insensitive by default so
   upstream works for them; the Linux build host used by MXE is
   case-sensitive, so the include fails. Rewrote to lowercase.

2. **Extended SAL1 shim in `streams.h`** — upstream already has a
   `#ifdef __MINGW32__` block that defines a *subset* of the
   SAL1 annotations (`__in`, `__out`, `__deref_in`, …). MinGW's
   `<sal.h>` ships the modern `_In_`/`_Out_` family but none of
   the SAL1 ones, so anything missing from the upstream block
   fails to compile. Extended the block to cover every SAL1
   annotation the BaseClasses headers reach for (`__deref_out`,
   `__in_bcount`, `__out_ecount_part`, …). All SAL macros are
   informational on MSVC too, so neutering them is sound.

Other than that, do not edit files in this directory. If a real
patch is needed, upstream it (or fork the source repo) and
re-vendor — keeping the snapshot pristine lets future bumps stay
mechanical.
