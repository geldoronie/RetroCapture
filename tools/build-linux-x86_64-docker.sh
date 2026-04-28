#!/bin/bash
set -e

# Build type: Release (default) or Debug
BUILD_TYPE="${1:-Release}"
FORCE_REBUILD=""
# Default ON: builds Docker são para distribuição, então o binário precisa rodar
# em CPUs sem AVX/AVX2. Quem quiser -march=native passa OFF explicitamente.
BUILD_COMPATIBLE="ON"

# Processar argumentos
for arg in "$@"; do
    case "$arg" in
        Release|Debug)
            BUILD_TYPE="$arg"
            ;;
        --rebuild)
            FORCE_REBUILD="--rebuild"
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
    echo "Uso: $0 [Release|Debug] [--rebuild] [ON|OFF]"
    echo "  Release - Build otimizado para produção (padrão)"
    echo "  Debug   - Build com símbolos de debug"
    echo "  --rebuild - Força rebuild completo da imagem Docker (mais lento)"
    echo "  ON|OFF  - Modo compatível (ON = padrão, sem AVX/AVX2 — roda em CPUs antigas;"
    echo "                              OFF = -march=native, só roda em CPUs como a do build host)"
    echo ""
    echo "Exemplos:"
    echo "  $0 Release              # Build Release padrão (compat ON, portátil)"
    echo "  $0 Release --rebuild    # Build Release com rebuild completo"
    echo "  $0 Release OFF          # Build Release nativo (-march=native, só pra uso local)"
    echo "  $0 Release --rebuild ON # Build Release compatível com rebuild"
    exit 1
fi

# Validar opção de compatibilidade
if [ "$BUILD_COMPATIBLE" != "ON" ] && [ "$BUILD_COMPATIBLE" != "OFF" ]; then
    echo "❌ Opção de compatibilidade inválida: $BUILD_COMPATIBLE"
    echo "   Use: ON ou OFF"
    exit 1
fi

echo "🐳 RetroCapture - Build para Linux usando Docker"
echo "================================================="
echo "📦 Build type: $BUILD_TYPE"
echo "🏗️  Arquitetura: x86_64 (amd64)"
if [ "$BUILD_COMPATIBLE" = "ON" ]; then
    echo "🔧 Modo compatível: ON (baseline x86-64-v2, sem AVX/AVX2 — recomendado para distribuição)"
else
    echo "⚡ Modo compatível: OFF (-march=native — só roda em CPUs equivalentes à do build host)"
fi
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
    $DOCKER_COMPOSE_CMD build $BUILD_FLAGS build-linux-x86_64
else
    $DOCKER_COMPOSE_CMD build build-linux-x86_64
fi

echo ""
echo "🔨 Compilando RetroCapture..."
echo ""

$DOCKER_COMPOSE_CMD run --rm \
    -e BUILD_TYPE="$BUILD_TYPE" \
    -e BUILD_COMPATIBLE_X86_64="$BUILD_COMPATIBLE" \
    build-linux-x86_64 > build-linux-x86_64.log 2>&1

echo ""
echo "✅ Concluído!"
echo "📁 Executável: ./build-linux-x86_64/bin/retrocapture"
