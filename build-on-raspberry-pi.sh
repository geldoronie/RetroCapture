#!/bin/bash
# Script para compilar RetroCapture diretamente na Raspberry Pi
# Isso garante compatibilidade com as bibliotecas do sistema

set -e

BUILD_TYPE="${1:-Release}"
BUILD_WITH_SDL2="${2:-}"

# Validar build type
if [ "$BUILD_TYPE" != "Release" ] && [ "$BUILD_TYPE" != "Debug" ]; then
    echo "❌ Build type inválido: $BUILD_TYPE"
    echo ""
    echo "Uso: $0 [Release|Debug] [SDL2]"
    echo "  Release - Build otimizado para produção (padrão)"
    echo "  Debug   - Build com símbolos de debug"
    echo "  SDL2    - Compilar com SDL2 (suporte DirectFB/framebuffer)"
    exit 1
fi

# Verificar se SDL2 foi solicitado
if [ "$BUILD_WITH_SDL2" = "SDL2" ] || [ "$BUILD_WITH_SDL2" = "sdl2" ]; then
    BUILD_WITH_SDL2="ON"
    echo "🔧 Build com SDL2 habilitado (DirectFB/framebuffer)"
else
    BUILD_WITH_SDL2="OFF"
fi

echo "🚀 Compilando RetroCapture na Raspberry Pi..."
echo "📦 Build type: $BUILD_TYPE"
echo ""

# Verificar se estamos no diretório correto
if [ ! -f "CMakeLists.txt" ]; then
    echo "❌ CMakeLists.txt não encontrado!"
    echo "   Execute este script a partir do diretório raiz do projeto"
    exit 1
fi

# Limpar CMakeCache.txt do diretório raiz se existir (pode ser de build anterior)
if [ -f "CMakeCache.txt" ]; then
    echo "🧹 Limpando CMakeCache.txt do diretório raiz..."
    rm -f CMakeCache.txt
    rm -rf CMakeFiles
fi

# Instalar dependências de desenvolvimento se necessário
echo "📦 Verificando dependências..."
NEED_INSTALL=false

if ! dpkg -l | grep -q libglfw3-dev; then
    NEED_INSTALL=true
fi

if [ "$BUILD_WITH_SDL2" = "ON" ] && ! dpkg -l | grep -q libsdl2-dev; then
    NEED_INSTALL=true
fi

if [ "$NEED_INSTALL" = "true" ]; then
    echo "   Instalando dependências de desenvolvimento..."
    DEPS="build-essential cmake pkg-config git libglfw3-dev libssl-dev libpng-dev"
    DEPS="$DEPS libavcodec-dev libavformat-dev libavutil-dev libswscale-dev"
    DEPS="$DEPS libswresample-dev libavfilter-dev libavdevice-dev libv4l-dev"
    DEPS="$DEPS libpulse-dev libx11-dev"
    
    if [ "$BUILD_WITH_SDL2" = "ON" ]; then
        DEPS="$DEPS libsdl2-dev"
    fi
    
    sudo apt-get update
    sudo apt-get install -y $DEPS
fi

# Criar diretório de build
BUILD_DIR="build-raspberry-pi"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Limpar cache se existir
if [ -f "CMakeCache.txt" ]; then
    echo "🧹 Limpando cache..."
    rm -f CMakeCache.txt
    rm -rf CMakeFiles
fi

if [ -d "_deps" ]; then
    echo "🧹 Limpando dependências anteriores..."
    rm -rf _deps
fi

echo "⚙️  Configurando CMake..."
if [ "$BUILD_WITH_SDL2" = "ON" ]; then
    echo "   🔧 Compilando com SDL2 (suporte DirectFB/framebuffer)"
    cmake .. \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DBUILD_WITH_SDL2=ON
else
    cmake .. -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
fi

echo ""
echo "🔨 Compilando..."
cmake --build . -j$(nproc)

echo ""
echo "✅ Build concluído!"
echo "📁 Executável: $(pwd)/bin/retrocapture"
echo ""
if [ "$BUILD_WITH_SDL2" = "ON" ]; then
    echo "💡 Este binário foi compilado com SDL2 (suporte DirectFB/framebuffer)"
    echo "   Para usar DirectFB: export SDL_VIDEODRIVER=directfb && ./bin/retrocapture"
    echo "   Para usar framebuffer: export SDL_VIDEODRIVER=fbcon && ./bin/retrocapture"
else
    echo "💡 Este binário está compilado com as bibliotecas do seu sistema"
    echo "   e deve ser totalmente compatível!"
fi
