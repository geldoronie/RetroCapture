#!/bin/bash
set -e

# Build type: Release (default) or Debug
BUILD_TYPE="${1:-Release}"
FORCE_REBUILD="${2:-}"

# Validar build type
if [ "$BUILD_TYPE" != "Release" ] && [ "$BUILD_TYPE" != "Debug" ]; then
    echo "❌ Build type inválido: $BUILD_TYPE"
    echo ""
    echo "Uso: $0 [Release|Debug] [--rebuild]"
    echo "  Release - Build otimizado para produção (padrão)"
    echo "  Debug   - Build com símbolos de debug"
    echo "  --rebuild - Força rebuild completo da imagem Docker (mais lento)"
    exit 1
fi

echo "🐳 RetroCapture - Build para Linux usando Docker"
echo "================================================="
echo "📦 Build type: $BUILD_TYPE"
echo "🔧 Base: Ubuntu 24.04 LTS (Noble Numbat) - FFmpeg 6.x (versão 60)"
echo "✅ Compatível com: Elementary OS 8.1 (Circe), Ubuntu 24.04+, etc."
echo ""

if ! command -v docker &> /dev/null; then
    echo "❌ Docker não está instalado!"
    echo "   Instale com: sudo apt-get install -y docker.io docker-compose"
    exit 1
fi

DOCKER_COMPOSE="docker-compose"
if ! command -v docker-compose &> /dev/null && docker compose version &> /dev/null; then
    DOCKER_COMPOSE="docker compose"
fi

# Detectar se precisa usar sudo (usuário não está no grupo docker)
DOCKER_CMD="docker"
DOCKER_COMPOSE_CMD="$DOCKER_COMPOSE"
if ! docker info &>/dev/null; then
    if sudo docker info &>/dev/null; then
        DOCKER_CMD="sudo docker"
        DOCKER_COMPOSE_CMD="sudo $DOCKER_COMPOSE"
        echo "⚠️  Usando sudo para Docker (usuário não está no grupo docker)"
        echo "   Para evitar sudo, execute: sudo usermod -aG docker $USER"
        echo "   Depois faça logout e login novamente."
        echo ""
    else
        echo "❌ Não foi possível acessar o Docker!"
        echo "   Verifique se o Docker está instalado e rodando:"
        echo "   sudo systemctl status docker"
        exit 1
    fi
fi

echo "📦 Construindo imagem Docker..."
if [ "$FORCE_REBUILD" = "--rebuild" ]; then
    echo "   🔄 Forçando rebuild completo (sem cache)..."
    echo "   Isso pode demorar vários minutos..."
    BUILD_FLAGS="--no-cache"
else
    echo "   Isso pode demorar alguns minutos na primeira vez..."
fi
echo "   (Usando Ubuntu 24.04 LTS - mesma base do Elementary OS 8.1)"
echo ""

if [ "$FORCE_REBUILD" = "--rebuild" ]; then
    $DOCKER_COMPOSE_CMD build $BUILD_FLAGS build-linux
else
    $DOCKER_COMPOSE_CMD build build-linux
fi

echo ""
echo "🔨 Compilando RetroCapture..."
echo ""

$DOCKER_COMPOSE_CMD run --rm -e BUILD_TYPE="$BUILD_TYPE" build-linux > build-linux.log 2>&1

echo ""
echo "✅ Concluído!"
echo "📁 Executável: ./build-linux/bin/retrocapture"
