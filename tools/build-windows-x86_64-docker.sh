#!/bin/bash
set -e

# Build type: Release (default) or Debug
BUILD_TYPE="${1:-Release}"
# Default ON: builds Docker são para distribuição (cross-compile MinGW também emite
# AVX2 com -march=native). Quem quiser -march=native passa OFF explicitamente.
BUILD_COMPATIBLE="ON"

# Processar argumentos
for arg in "$@"; do
    case "$arg" in
        Release|Debug)
            BUILD_TYPE="$arg"
            ;;
        ON|OFF)
            BUILD_COMPATIBLE="$arg"
            ;;
        *)
            if [ "$arg" != "$BUILD_TYPE" ]; then
                echo "⚠️  Argumento desconhecido ignorado: $arg"
            fi
            ;;
    esac
done

# Se BUILD_COMPATIBLE_X86_64 estiver definido como variável de ambiente, usar ela
if [ -n "$BUILD_COMPATIBLE_X86_64" ]; then
    BUILD_COMPATIBLE="$BUILD_COMPATIBLE_X86_64"
fi

# Validar build type
if [ "$BUILD_TYPE" != "Release" ] && [ "$BUILD_TYPE" != "Debug" ]; then
    echo "❌ Build type inválido: $BUILD_TYPE"
    echo ""
    echo "Uso: $0 [Release|Debug] [ON|OFF]"
    echo "  Release - Build otimizado para produção (padrão)"
    echo "  Debug   - Build com símbolos de debug"
    echo "  ON|OFF  - Modo compatível (ON = padrão, sem AVX/AVX2 — roda em CPUs antigas;"
    echo "                              OFF = -march=native, só pra uso local)"
    exit 1
fi

# Validar opção de compatibilidade
if [ "$BUILD_COMPATIBLE" != "ON" ] && [ "$BUILD_COMPATIBLE" != "OFF" ]; then
    echo "❌ Opção de compatibilidade inválida: $BUILD_COMPATIBLE"
    echo "   Use: ON ou OFF"
    exit 1
fi

echo "🐳 RetroCapture - Build para Windows usando Docker"
echo "==================================================="
echo "📦 Build type: $BUILD_TYPE"
echo "🏗️  Arquitetura: x86_64 (amd64)"
if [ "$BUILD_COMPATIBLE" = "ON" ]; then
    echo "🔧 Modo compatível: ON (baseline x86-64-v2, sem AVX/AVX2 — recomendado para distribuição)"
else
    echo "⚡ Modo compatível: OFF (-march=native — só roda em CPUs equivalentes à do build host)"
fi
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

$DOCKER_COMPOSE build build-windows-x86_64

echo ""
echo "🔨 Compilando RetroCapture..."
echo ""

$DOCKER_COMPOSE run --rm \
    -e BUILD_TYPE="$BUILD_TYPE" \
    -e BUILD_COMPATIBLE_X86_64="$BUILD_COMPATIBLE" \
    build-windows-x86_64 > build-windows-x86_64.log 2>&1

echo ""
echo "✅ Concluído!"
echo "📁 Executável: ./build-windows-x86_64/bin/retrocapture.exe"
