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

# Limpar CMakeCache.txt do diretório raiz se existir (pode ser de build anterior)
if [ -f "CMakeCache.txt" ]; then
    echo "🧹 Limpando CMakeCache.txt do diretório raiz..."
    rm -f CMakeCache.txt
    rm -rf CMakeFiles
fi

# Configurar Git ANTES de qualquer operação (resolve "dubious ownership" no Docker)
# Isso deve ser feito antes de entrar no diretório de build
echo "⚙️  Configurando Git..."
git config --global --add safe.directory '*' || true

# Criar diretório de build (limpar cache CMake se existir)
BUILD_DIR="build-linux-arm64v8"
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

# Configurar ccache se disponível
if command -v ccache &> /dev/null; then
    export CC="ccache gcc"
    export CXX="ccache g++"
    echo "⚡ ccache habilitado para acelerar builds incrementais"
    ccache --show-stats || true
fi

echo "⚙️  Configurando CMake..."
# BUILD_WITH_SDL2 pode ser passado via variável de ambiente
BUILD_WITH_SDL2="${BUILD_WITH_SDL2:-OFF}"

# Detectar número de CPUs disponíveis
# Em containers Docker, pode ser limitado, então usar nproc ou variável de ambiente
NUM_CPUS="${NUM_CPUS:-$(nproc)}"
# Para Ryzen 9 5900x (12 cores, 24 threads), usar até 20 jobs para aproveitar melhor
# Mas respeitar limites do container
if [ "$NUM_CPUS" -gt 20 ]; then
    NUM_CPUS=20
fi

if [ "$BUILD_WITH_SDL2" = "ON" ]; then
    echo "   🔧 Compilando com SDL2 (suporte DirectFB/framebuffer)"
    cmake .. \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DBUILD_WITH_SDL2=ON \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
else
    cmake .. \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
fi

echo ""
echo "🔨 Compilando com $NUM_CPUS jobs paralelos..."
cmake --build . -j$NUM_CPUS

# Mostrar estatísticas do ccache
if command -v ccache &> /dev/null; then
    echo ""
    echo "📊 Estatísticas do ccache:"
    ccache --show-stats || true
fi

echo ""
echo "✅ Build concluído!"
echo ""
echo "📁 Executável: $(pwd)/bin/retrocapture"
echo ""
if [ "$BUILD_WITH_SDL2" = "ON" ]; then
    echo "💡 Este binário foi compilado com SDL2 (suporte DirectFB/framebuffer)"
    echo "   Para usar DirectFB: export SDL_VIDEODRIVER=directfb && ./bin/retrocapture"
    echo "   Para usar framebuffer: export SDL_VIDEODRIVER=fbcon && ./bin/retrocapture"
fi
