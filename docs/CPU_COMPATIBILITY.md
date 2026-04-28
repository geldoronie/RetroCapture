# CPU Compatibility

This document describes how RetroCapture handles CPU instruction-set portability
across distributed binaries, why it matters, and how to control the compile
flags for each supported architecture.

## Why this matters

Compiling with `-march=native` lets the compiler emit any instruction set the
**build host** supports (AVX2 on Intel Haswell+, SVE on some ARM64 CPUs, etc.).
That is great for binaries you only run on the same machine, but for any
binary you publish or ship to other machines it produces a runtime crash on
older or different CPUs:

```
zsh: illegal hardware instruction (core dumped) ./retrocapture
```

Because of that, RetroCapture distinguishes two compile modes:

- **Compatible mode** (default for all Docker / distribution builds) — emits
  only a portable baseline of instructions for the target architecture.
- **Native mode** (opt-in) — emits whatever the local CPU supports, for
  best performance when you build on the same machine that runs the binary.

The CMake options that control this are:

| Architecture        | Option                       | Default | Effect when ON                             |
|---------------------|------------------------------|---------|--------------------------------------------|
| x86_64 (Linux/Win)  | `BUILD_COMPATIBLE_X86_64`    | `OFF`   | `-march=x86-64-v2` baseline, no AVX/AVX2   |
| ARM64 (aarch64)     | `BUILD_COMPATIBLE_ARM64`     | `OFF`   | `-march=armv8-a` baseline, no SVE/crypto   |
| ARM32 (armhf)       | (always portable, see below) | n/a     | `-march=armv7-a -mfpu=neon-vfpv4`          |

The CMake defaults are `OFF` so direct `cmake ..` invocations behave like
before. The Docker build scripts flip the default to `ON` because their output
is meant for distribution.

## Architectures

### x86_64 (Linux and Windows)

**Compatible baseline (`BUILD_COMPATIBLE_X86_64=ON`):**

- On GCC ≥ 11 / Clang ≥ 12: `-march=x86-64-v2 -mtune=generic`. The
  `x86-64-v2` micro-architecture level (SSE3, SSSE3, SSE4.1, SSE4.2, POPCNT)
  is supported by every Intel CPU since Nehalem (2008) and every AMD CPU
  since Bulldozer (2011).
- On older toolchains (auto-detected via `check_cxx_compiler_flag`):
  `-march=x86-64 -mtune=generic -msse4.2 -mno-avx -mno-avx2`. Same effective
  baseline, expressed as the equivalent explicit flags.

CPUs that work with the compatible baseline include AMD A8 PRO-7600B (Kaveri,
no AVX2), Intel Core 2/i3/i5/i7 from Nehalem onward, and every modern CPU.

**Native mode (`BUILD_COMPATIBLE_X86_64=OFF`):**

- `-march=native`. The binary may include AVX, AVX2, AVX-512, etc., depending
  on the build host CPU. It will only run on CPUs that implement at least
  the same instruction set extensions.

The Windows cross-compile (MXE/MinGW) uses the **same** option, because
`-march=native` queries the build host (Linux) and bakes those instructions
into the resulting `retrocapture.exe`.

### ARM64 / aarch64 (Raspberry Pi 4/5, generic ARMv8 boards)

**Compatible baseline (`BUILD_COMPATIBLE_ARM64=ON`):**

- `-march=armv8-a` for both `retrocapture` and the bundled ImGui static
  library. Pure ARMv8-A with no optional extensions, so the binary runs on
  every ARMv8 CPU from Cortex-A53 (Pi 3) onwards, including Pi 4
  (Cortex-A72) and Pi 5 (Cortex-A76).

This default matters specifically for builds run inside the `arm64v8/debian`
Docker image on an x86_64 host: under `qemu-user-static` the emulated CPU
typically advertises a "max" model that includes SVE / SVE2 / dotprod /
crypto. With `-march=native` GCC happily emits those instructions, and the
resulting binary segfaults on real Pi hardware.

**Native mode (`BUILD_COMPATIBLE_ARM64=OFF`):**

- `-march=native`. Useful only when you are building on the exact same board
  you intend to run on (e.g. `tools/build-on-raspberry-pi.sh` on a Pi 5).

### ARM32 / armhf (Raspberry Pi 2/3, ARMv7-A boards)

The ARM32 path is always built with a fixed portable baseline:

```
-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard
```

That covers Cortex-A7 and Cortex-A53 in 32-bit mode (Pi 2, Pi 3) as well as
Pi 4/5 booted with a 32-bit OS. It excludes ARMv6 boards (Pi 1, Pi Zero
non-2W). There is no `BUILD_COMPATIBLE_ARM32` option because the baseline is
already conservative enough.

## Build script defaults

Every Docker build script defaults to compatible mode. You can opt out for
local-only builds:

| Script                                       | Compat default | How to opt out                                   |
|----------------------------------------------|----------------|--------------------------------------------------|
| `tools/build-linux-x86_64-docker.sh`         | ON             | `./tools/build-linux-x86_64-docker.sh Release OFF` |
| `tools/build-windows-x86_64-docker.sh`       | ON             | `./tools/build-windows-x86_64-docker.sh Release OFF` |
| `tools/build-linux-arm64v8-docker.sh`        | ON             | `./tools/build-linux-arm64v8-docker.sh Release --native` |
| `tools/build-linux-arm32v7-docker.sh`        | n/a            | always portable                                  |
| `tools/build-on-raspberry-pi.sh`             | uses CMake default (OFF) | already on the target board                      |

The inner Docker scripts read the same option from the environment:
`BUILD_COMPATIBLE_X86_64` for x86_64 / Windows, `BUILD_COMPATIBLE_ARM64` for
aarch64.

Plain CMake invocations keep the legacy `-march=native` default. To reproduce
a distribution build directly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_COMPATIBLE_X86_64=ON
cmake --build build -j$(nproc)
```

## Diagnosing "illegal hardware instruction"

Use the diagnostic script:

```bash
./tools/diagnose-cpu-compatibility.sh [path-to-binary]
```

It prints the instruction-set extensions found in the binary, the extensions
your CPU supports, and a verdict. The script currently only inspects x86
extensions (AVX, AVX2, AVX-512, SSE4.2); ARM diagnostics are out of scope.

If the binary uses extensions that your CPU does not support, rebuild with
the compatible baseline (or grab a distribution build, which is already
portable by default).

## Performance impact

For RetroCapture specifically the performance gap between native and
compatible builds is small. The hot paths (YUYV → RGB conversion, video
encoding) are inside `libswscale` / `libavcodec`, which already perform
runtime CPU dispatch internally — they pick the best AVX2 / NEON path
available regardless of how the surrounding RetroCapture binary was
compiled. Our own C++ code does not contain explicit SIMD intrinsics, so
auto-vectorization of a few simple loops is the only thing the compatible
build gives up.
