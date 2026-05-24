#!/bin/bash
set -e

echo "🍎 RetroCapture - Instalação de Dependências para macOS"
echo "======================================================"
echo ""

# Verificar se Homebrew está instalado
if ! command -v brew &> /dev/null; then
    echo "❌ Homebrew não está instalado!"
    echo ""
    echo "   Instale o Homebrew primeiro:"
    echo "   /bin/bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\""
    echo ""
    exit 1
fi

echo "✅ Homebrew encontrado"
echo ""

# Atualizar Homebrew
echo "📦 Atualizando Homebrew..."
brew update

echo ""
echo "📦 Instalando dependências..."
echo ""

# Dependências principais
echo "   - CMake (build system)"
brew install cmake

echo "   - GLFW (window management)"
brew install glfw

echo "   - FFmpeg (streaming/encoding)"
brew install ffmpeg

echo "   - libpng (image loading)"
brew install libpng

echo "   - pkg-config (dependency detection)"
brew install pkg-config

echo ""
echo "✅ Todas as dependências foram instaladas!"
echo ""
echo "📝 Dependências instaladas:"
echo "   - CMake"
echo "   - GLFW"
echo "   - FFmpeg (libavcodec, libavformat, libavutil, libswscale, libswresample)"
echo "   - libpng"
echo "   - pkg-config"
echo ""
echo "🎯 Próximo passo: Execute o build:"
echo "   ./tools/build-macos.sh"
echo ""
