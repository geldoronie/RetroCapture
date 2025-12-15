#!/bin/bash
set -e

echo "🐳 RetroCapture - Build para Linux usando Docker"
echo "================================================="
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

$DOCKER_COMPOSE run --rm build-linux > build-linux.log 2>&1

echo ""
echo "✅ Concluído!"
echo "📁 Executável: ./build-linux/bin/retrocapture"
