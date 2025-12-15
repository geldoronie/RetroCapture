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

echo "🐳 RetroCapture - Build para Windows usando Docker"
echo "==================================================="
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
echo "   Isso pode demorar 30-60 minutos na primeira vez..."
echo ""

$DOCKER_COMPOSE build build-windows

echo ""
echo "🔨 Compilando RetroCapture..."
echo ""

$DOCKER_COMPOSE run --rm -e BUILD_TYPE="$BUILD_TYPE" build-windows > build-windows.log 2>&1

echo ""
echo "✅ Concluído!"
echo "📁 Executável: ./build-windows/bin/retrocapture.exe"
