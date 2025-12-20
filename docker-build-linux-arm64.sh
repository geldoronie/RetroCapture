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

echo "🚀 Compilando RetroCapture para Linux ARM64 (Raspberry Pi 4/5)..."
echo "📦 Build type: $BUILD_TYPE"
echo "🏗️  Arquitetura: ARM64 (aarch64)"
echo ""

# Verificar se estamos no diretório correto
if [ ! -f "CMakeLists.txt" ]; then
    echo "❌ CMakeLists.txt não encontrado!"
    exit 1
fi

# Configurar Git ANTES de qualquer operação (resolve "dubious ownership" no Docker)
# Isso deve ser feito antes de entrar no diretório de build
echo "⚙️  Configurando Git..."
git config --global --add safe.directory '*' || true

# Criar diretório de build (limpar cache CMake se existir)
BUILD_DIR="build-linux-arm64"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Limpar cache do CMake se existir (pode ter sido criado fora do container)
if [ -f "CMakeCache.txt" ]; then
    echo "🧹 Limpando cache do CMake..."
    rm -f CMakeCache.txt
    rm -rf CMakeFiles
fi

# Limpar diretório _deps se existir (pode ter sido criado com permissões incorretas)
# Isso garante que o FetchContent baixe tudo do zero com as permissões corretas
if [ -d "_deps" ]; then
    echo "🧹 Limpando dependências anteriores..."
    rm -rf _deps
fi

echo "⚙️  Configurando CMake..."
# BUILD_WITH_SDL2 pode ser passado via variável de ambiente
BUILD_WITH_SDL2="${BUILD_WITH_SDL2:-OFF}"
if [ "$BUILD_WITH_SDL2" = "ON" ]; then
    echo "   🔧 Compilando com SDL2 (suporte DirectFB/framebuffer)"
    cmake .. \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DBUILD_WITH_SDL2=ON
else
    cmake .. \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
fi

echo ""
echo "🔨 Compilando..."
cmake --build . -j$(nproc)

echo ""
echo "✅ Build concluído!"
echo ""
echo "📁 Executável: $(pwd)/bin/retrocapture"
