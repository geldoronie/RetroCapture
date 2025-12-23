#!/bin/bash
set -e

# Build type: Release (default) or Debug
BUILD_TYPE="${BUILD_TYPE:-Release}"

# Validar build type
if [ "$BUILD_TYPE" != "Release" ] && [ "$BUILD_TYPE" != "Debug" ]; then
    echo "❌ Build type inválido: $BUILD_TYPE"
    echo "   Use: Release ou Debug"
    exit 1
fi

echo "🚀 Compilando RetroCapture para Windows x86_64..."
echo "📦 Build type: $BUILD_TYPE"
echo "🏗️  Arquitetura: x86_64 (amd64)"
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
-DCMAKE_BUILD_TYPE="$BUILD_TYPE"

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

