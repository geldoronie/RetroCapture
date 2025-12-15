#!/bin/bash
set -e

# Build type: Release (default) or Debug
BUILD_TYPE="${1:-Release}"

# Validar build type
if [ "$BUILD_TYPE" != "Release" ] && [ "$BUILD_TYPE" != "Debug" ]; then
    echo "❌ Build type inválido: $BUILD_TYPE"
    echo ""
    echo "Uso: $0 [Release|Debug]"
    echo "  Release - Build otimizado para produção (padrão)"
    echo "  Debug   - Build com símbolos de debug"
    exit 1
fi

echo "🐳 RetroCapture - Build para Linux usando Docker"
echo "================================================="
echo "📦 Build type: $BUILD_TYPE"
echo ""

if ! command -v docker &> /dev/null; then
    echo "❌ Docker não está instalado!"
    exit 1
fi

DOCKER_COMPOSE="docker-compose"
if ! command -v docker-compose &> /dev/null && docker compose version &> /dev/null; then
    DOCKER_COMPOSE="docker compose"
fi

echo "📦 Construindo imagem Docker..."
echo "   Isso pode demorar alguns minutos na primeira vez..."
echo ""

$DOCKER_COMPOSE build build-linux

echo ""
echo "🔨 Compilando RetroCapture..."
echo ""

$DOCKER_COMPOSE run --rm -e BUILD_TYPE="$BUILD_TYPE" build-linux > build-linux.log 2>&1

echo ""
echo "✅ Concluído!"
echo "📁 Executável: ./build-linux/bin/retrocapture"
