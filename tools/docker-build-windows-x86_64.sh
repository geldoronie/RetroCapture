#!/bin/bash
set -e

# Build type: Release (default) or Debug
BUILD_TYPE="${BUILD_TYPE:-Release}"

# Compatibilidade: builds Docker são pra distribuição, então default ON
# (baseline x86-64-v2, sem AVX/AVX2). Para -march=native passe BUILD_COMPATIBLE_X86_64=OFF.
# A flag aqui é a MESMA do build Linux (CMake só conhece BUILD_COMPATIBLE_X86_64,
# independente do alvo): o cross-compile MinGW também emite AVX2 com -march=native.
BUILD_COMPATIBLE="${BUILD_COMPATIBLE_X86_64:-ON}"

# Validar build type
if [ "$BUILD_TYPE" != "Release" ] && [ "$BUILD_TYPE" != "Debug" ]; then
    echo "❌ Build type inválido: $BUILD_TYPE"
    echo "   Use: Release ou Debug"
    exit 1
fi

# Validar opção de compatibilidade
if [ "$BUILD_COMPATIBLE" != "ON" ] && [ "$BUILD_COMPATIBLE" != "OFF" ]; then
    echo "❌ BUILD_COMPATIBLE_X86_64 inválido: $BUILD_COMPATIBLE"
    echo "   Use: ON ou OFF"
    exit 1
fi

echo "🚀 Compilando RetroCapture para Windows x86_64..."
echo "📦 Build type: $BUILD_TYPE"
echo "🏗️  Arquitetura: x86_64 (amd64)"
if [ "$BUILD_COMPATIBLE" = "ON" ]; then
    echo "🔧 Modo compatível: ON (baseline x86-64-v2, sem AVX/AVX2 — recomendado para distribuição)"
else
    echo "⚡ Modo compatível: OFF (-march=native — só roda em CPUs equivalentes à do build host)"
fi
echo ""

# dockcross monta o código em /work
cd /work

if [ ! -f "CMakeLists.txt" ]; then
    echo "❌ CMakeLists.txt não encontrado!"
    exit 1
fi

mkdir -p build-windows-x86_64
cd build-windows-x86_64

echo "⚙️  Configurando CMake..."
# dockcross usa MXE em /usr/src/mxe com target static
export PKG_CONFIG_PATH=/usr/src/mxe/usr/x86_64-w64-mingw32.static/lib/pkgconfig

# Virtcam DLL (#85 Phase 2) — defaults ON for the Windows cross-build
# because the host-side sink is meaningless without the DirectShow
# filter that exposes it to consumers. Set BUILD_VIRTCAM_DSHOW=OFF
# explicitly when bisecting unrelated build issues.
BUILD_VIRTCAM_DSHOW="${BUILD_VIRTCAM_DSHOW:-ON}"

if [ "$BUILD_COMPATIBLE" = "ON" ]; then
    cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=/usr/src/mxe/usr/x86_64-w64-mingw32.static/share/cmake/mxe-conf.cmake \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DBUILD_COMPATIBLE_X86_64=ON \
    -DRETROCAPTURE_BUILD_VIRTCAM_DSHOW_FILTER="$BUILD_VIRTCAM_DSHOW"
else
    cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=/usr/src/mxe/usr/x86_64-w64-mingw32.static/share/cmake/mxe-conf.cmake \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DRETROCAPTURE_BUILD_VIRTCAM_DSHOW_FILTER="$BUILD_VIRTCAM_DSHOW"
fi

echo ""
echo "🔨 Compilando..."
# CMake 3.10 precisa de -- para passar -j ao make
cmake --build . -- -j$(nproc)

echo ""
echo "✅ Build concluído!"
echo ""
echo "📁 Executável: $(pwd)/bin/retrocapture.exe"

# Opcional: Gerar instalador se CPack estiver configurado
# Descomente as linhas abaixo para gerar instalador automaticamente após o build
# if command -v cpack &> /dev/null && command -v makensis &> /dev/null; then
#     echo ""
#     echo "📦 Gerando instalador Windows..."
#     cpack -G NSIS
#     if [ $? -eq 0 ]; then
#         echo "✅ Instalador gerado: $(ls -1 RetroCapture-*.exe 2>/dev/null | head -1)"
#     else
#         echo "⚠️  Falha ao gerar instalador (opcional)"
#     fi
# fi

