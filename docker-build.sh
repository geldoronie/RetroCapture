#!/bin/bash
set -e

echo "🚀 Compilando RetroCapture para Windows..."
echo ""

# dockcross monta o código em /work
cd /work

if [ ! -f "CMakeLists.txt" ]; then
    echo "❌ CMakeLists.txt não encontrado!"
    exit 1
fi

mkdir -p build-windows
cd build-windows

echo "⚙️  Configurando CMake..."
# dockcross usa MXE em /usr/src/mxe com target static
export PKG_CONFIG_PATH=/usr/src/mxe/usr/x86_64-w64-mingw32.static/lib/pkgconfig
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=/usr/src/mxe/usr/x86_64-w64-mingw32.static/share/cmake/mxe-conf.cmake \
    -DCMAKE_BUILD_TYPE=Release

echo ""
echo "🔨 Compilando..."
# CMake 3.10 precisa de -- para passar -j ao make
cmake --build . -- -j$(nproc)

echo ""
echo "✅ Build concluído!"
echo ""
echo "📁 Executável: $(pwd)/bin/retrocapture.exe"

