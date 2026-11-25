#!/bin/bash
# Builds the pinned ncnn release with Vulkan for the RIFE frame-generation
# backend (#248) and installs it to PREFIX. Used by the Docker build images
# (Linux x86_64, Windows/MXE) and the macOS workflow; a local build can use
# it too and pass -DRIFE_NCNN_DIR=<prefix> to CMake.
#
#   tools/build-ncnn.sh <prefix> [extra cmake args...]
#
# Env: CMAKE (cmake binary, default "cmake"), JOBS (default nproc),
#      NCNN_WORK (scratch dir, default a mktemp dir, removed afterwards).
#
# Only what RIFE needs is built: the static library with Vulkan compute and
# its bundled glslang (shaders are compiled at run time), no tools,
# examples, benchmark, OpenMP or x86 AVX kernels (the CPU path is not used;
# keeps the binary small and portable). NCNN_SIMPLEVK loads the Vulkan
# loader at run time, so nothing Vulkan is needed to build or link.
set -euo pipefail

NCNN_VERSION=20260526
NCNN_SHA256=754659d6fe65545cf2ef4483ffb84526fea631f8764c44b150f1601d0fb4004b
NCNN_URL="https://github.com/Tencent/ncnn/releases/download/${NCNN_VERSION}/ncnn-${NCNN_VERSION}-full-source.zip"

PREFIX="${1:?usage: $0 <prefix> [extra cmake args...]}"
shift
CMAKE="${CMAKE:-cmake}"
if [ -z "${JOBS:-}" ]; then
    JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
fi
WORK="${NCNN_WORK:-$(mktemp -d)}"
cleanup() { [ -n "${NCNN_WORK:-}" ] || rm -rf "$WORK"; }
trap cleanup EXIT

echo "ncnn ${NCNN_VERSION} (Vulkan) -> ${PREFIX}"
mkdir -p "$WORK"
if command -v curl >/dev/null; then
    curl -fsSL -o "$WORK/ncnn.zip" "$NCNN_URL"
else
    wget -q -O "$WORK/ncnn.zip" "$NCNN_URL"
fi
if command -v sha256sum >/dev/null; then
    echo "${NCNN_SHA256}  $WORK/ncnn.zip" | sha256sum -c -
else
    echo "${NCNN_SHA256}  $WORK/ncnn.zip" | shasum -a 256 -c -
fi
rm -rf "$WORK/src" "$WORK/build"
mkdir -p "$WORK/src"
(cd "$WORK/src" && unzip -q ../ncnn.zip)

# GCC 5 (MXE, Windows cross build) mis-resolves `StorageClass::X` where a
# parameter of the same name shadows the enum; qualifying it is valid C++
# for every compiler.
sed -i.bak 's/(int)StorageClass::/(int)spv::StorageClass::/' "$WORK/src/glslang/SPIRV/doc.cpp"

"$CMAKE" -S "$WORK/src" -B "$WORK/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DNCNN_VULKAN=ON \
    -DNCNN_SIMPLEVK=ON \
    -DNCNN_SHARED_LIB=OFF \
    -DNCNN_OPENMP=OFF \
    -DNCNN_AVX=OFF \
    -DNCNN_C_API=OFF \
    -DNCNN_PIXEL_DRAWING=OFF \
    -DNCNN_PIXEL_AFFINE=OFF \
    -DNCNN_PIXEL_ROTATE=OFF \
    -DNCNN_BUILD_TOOLS=OFF \
    -DNCNN_BUILD_EXAMPLES=OFF \
    -DNCNN_BUILD_BENCHMARK=OFF \
    -DNCNN_BUILD_TESTS=OFF \
    -DNCNN_PYTHON=OFF \
    "$@"
"$CMAKE" --build "$WORK/build" -j "$JOBS"
"$CMAKE" --install "$WORK/build"
echo "ncnn installed to ${PREFIX}"
