#!/bin/bash
# Script para instalar dependências do RetroCapture na Raspberry Pi
# Funciona com diferentes versões do Debian/Ubuntu

set -e

echo "🔍 Detectando versão do sistema..."
if [ -f /etc/os-release ]; then
    . /etc/os-release
    echo "   Sistema: $PRETTY_NAME"
    echo "   Versão: $VERSION_ID"
fi

echo ""
echo "📦 Instalando dependências do RetroCapture..."
echo ""

# Atualizar lista de pacotes
sudo apt-get update

# Dependências básicas do sistema e gráficas
echo "Instalando dependências básicas e gráficas..."
sudo apt-get install -y \
    libgl1-mesa-dev \
    libglfw3-dev \
    libglfw3 \
    libx11-6 \
    libx11-dev \
    libxrandr-dev \
    libxinerama-dev \
    libxcursor-dev \
    libxi-dev \
    libpulse0 \
    libpulse-dev \
    xorg-dev

# FFmpeg - tentar detectar versão disponível
echo ""
echo "Instalando FFmpeg..."
if apt-cache show libavcodec61 &>/dev/null; then
    echo "   Usando FFmpeg 6.1 (Debian Trixie)"
    sudo apt-get install -y \
        libavcodec61 \
        libavformat61 \
        libavutil59 \
        libswscale8 \
        libswresample5
elif apt-cache show libavcodec60 &>/dev/null; then
    echo "   Usando FFmpeg 6.0"
    sudo apt-get install -y \
        libavcodec60 \
        libavformat60 \
        libavutil58 \
        libswscale7 \
        libswresample4
elif apt-cache show libavcodec59 &>/dev/null; then
    echo "   Usando FFmpeg 5.9"
    sudo apt-get install -y \
        libavcodec59 \
        libavformat59 \
        libavutil57 \
        libswscale6 \
        libswresample4
else
    echo "   Instalando versão genérica do FFmpeg"
    sudo apt-get install -y \
        libavcodec-dev \
        libavformat-dev \
        libavutil-dev \
        libswscale-dev \
        libswresample-dev
fi

# PNG - versão genérica funciona
echo ""
echo "Instalando libpng..."
if apt-cache show libpng16-16t64 &>/dev/null; then
    sudo apt-get install -y libpng16-16t64
elif apt-cache show libpng16-16 &>/dev/null; then
    sudo apt-get install -y libpng16-16
else
    sudo apt-get install -y libpng16
fi

# OpenSSL - versão genérica
echo ""
echo "Instalando OpenSSL..."
if apt-cache show libssl3t64 &>/dev/null; then
    sudo apt-get install -y libssl3t64
elif apt-cache show libssl3 &>/dev/null; then
    sudo apt-get install -y libssl3
else
    sudo apt-get install -y libssl-dev
fi

# libcrypto geralmente vem com libssl
if ! dpkg -l | grep -q libcrypto; then
    if apt-cache show libcrypto3t64 &>/dev/null; then
        sudo apt-get install -y libcrypto3t64
    elif apt-cache show libcrypto3 &>/dev/null; then
        sudo apt-get install -y libcrypto3
    fi
fi

echo ""
echo "✅ Dependências instaladas!"
echo ""
echo "📋 Para verificar as dependências do binário, use:"
echo "   readelf -d retrocapture | grep NEEDED"
echo "   ou"
echo "   objdump -p retrocapture | grep NEEDED"
echo ""
echo "⚠️  IMPORTANTE: Se você estiver rodando via SSH sem display:"
echo "   1. Configure DISPLAY: export DISPLAY=:0"
echo "   2. Ou use Xvfb: sudo apt-get install -y xvfb && xvfb-run -a ./retrocapture"
echo ""
echo "📖 Veja docs/RASPBERRY_PI_SETUP.md para mais informações sobre problemas comuns"

