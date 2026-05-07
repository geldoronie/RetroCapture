#!/bin/bash
# Script para instalar dependências do RetroCapture no Manjaro/Arch Linux
# Uso: ./tools/install-deps-manjaro.sh

set -e

echo "🔍 Detectando sistema..."
if [ -f /etc/os-release ]; then
    . /etc/os-release
    echo "   Sistema: $PRETTY_NAME"
    echo "   Versão: $VERSION_ID"
fi

# Verificar se está rodando como root (necessário para pacman)
if [ "$EUID" -eq 0 ]; then
    echo "❌ Não execute este script como root/sudo."
    echo "   O script pedirá senha quando necessário."
    exit 1
fi

echo ""
echo "📦 Instalando dependências do RetroCapture no Manjaro/Arch Linux..."
echo ""

# Atualizar banco de dados de pacotes
echo "🔄 Atualizando banco de dados de pacotes..."
sudo pacman -Sy

# Ferramentas de build
echo ""
echo "🔨 Instalando ferramentas de build..."
sudo pacman -S --needed --noconfirm \
    base-devel \
    cmake \
    pkgconf \
    git

# Dependências gráficas e de janela
echo ""
echo "🖼️  Instalando dependências gráficas..."
sudo pacman -S --needed --noconfirm \
    mesa \
    libgl \
    glfw \
    libx11 \
    libxrandr \
    libxinerama \
    libxcursor \
    libxi

# SDL2 (opcional, para suporte a DirectFB/framebuffer)
echo ""
echo "🎮 Instalando SDL2 (opcional, para framebuffer/DirectFB)..."
sudo pacman -S --needed --noconfirm sdl2

# PNG
echo ""
echo "🖼️  Instalando libpng..."
sudo pacman -S --needed --noconfirm libpng

# FFmpeg
echo ""
echo "🎬 Instalando FFmpeg..."
sudo pacman -S --needed --noconfirm \
    ffmpeg

# V4L2 (Video4Linux2 - para captura de vídeo no Linux)
echo ""
echo "📹 Instalando V4L2..."
sudo pacman -S --needed --noconfirm \
    v4l-utils

# PulseAudio (para captura de áudio no Linux)
echo ""
echo "🔊 Instalando PulseAudio..."
sudo pacman -S --needed --noconfirm \
    libpulse

# OpenSSL (para suporte HTTPS)
echo ""
echo "🔒 Instalando OpenSSL..."
sudo pacman -S --needed --noconfirm \
    openssl

echo ""
echo "✅ Todas as dependências foram instaladas!"
echo ""
echo "📋 Resumo das dependências instaladas:"
echo "   • Ferramentas de build: base-devel, cmake, pkgconf, git"
echo "   • Gráficas: mesa, libgl, glfw, libx11, libxrandr, libxinerama, libxcursor, libxi"
echo "   • SDL2: sdl2 (opcional)"
echo "   • Imagens: libpng"
echo "   • Mídia: ffmpeg"
echo "   • Captura de vídeo: v4l-utils"
echo "   • Captura de áudio: libpulse"
echo "   • Segurança: openssl"
echo ""
echo "🚀 Agora você pode compilar o RetroCapture:"
echo "   mkdir -p build-linux-x86_64"
echo "   cd build-linux-x86_64"
echo "   cmake .. -DCMAKE_BUILD_TYPE=Release"
echo "   cmake --build . -j\$(nproc)"
echo ""
echo "💡 Dica: Se você quiser usar SDL2 em vez de GLFW (para framebuffer/DirectFB),"
echo "   use: cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_WITH_SDL2=ON"
echo ""
