#!/bin/bash
set -e

echo "🐳 RetroCapture - Build para Windows usando Docker"
echo "==================================================="
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

$DOCKER_COMPOSE run --rm build-windows > build-windows.log 2>&1

echo ""
echo "✅ Concluído!"
echo "📁 Executável: ./build-windows/bin/retrocapture.exe"
