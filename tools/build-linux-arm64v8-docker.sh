#!/bin/bash
set -e

# Build type: Release (default) or Debug
BUILD_TYPE="${1:-Release}"
FORCE_REBUILD=""
BUILD_WITH_SDL2_ARG=""
# Default ON: builds Docker são para distribuição (sob qemu, -march=native pode
# emitir SVE/SVE2 que crasha em hardware real). Quem quiser nativo passa --native.
BUILD_COMPATIBLE="ON"

# Processar argumentos
# Note: SDL2 ativa SDL2; --native desativa o modo compatível (vai pra -march=native);
# ON/OFF puro continua significando SDL2 (compat. retroativa com o uso anterior).
for arg in "$@"; do
    case "$arg" in
        Release|Debug)
            BUILD_TYPE="$arg"
            ;;
        --rebuild)
            FORCE_REBUILD="--rebuild"
            ;;
        SDL2|sdl2)
            BUILD_WITH_SDL2_ARG="$arg"
            ;;
        --native|native)
            BUILD_COMPATIBLE="OFF"
            ;;
        --compat|compat)
            BUILD_COMPATIBLE="ON"
            ;;
        *)
            if [ "$arg" != "$BUILD_TYPE" ]; then
                echo "⚠️  Argumento desconhecido: $arg (ignorado)"
            fi
            ;;
    esac
done

# Se BUILD_COMPATIBLE_ARM64 estiver definido como variável de ambiente, usar ela
if [ -n "$BUILD_COMPATIBLE_ARM64" ]; then
    BUILD_COMPATIBLE="$BUILD_COMPATIBLE_ARM64"
fi

# Validar build type
if [ "$BUILD_TYPE" != "Release" ] && [ "$BUILD_TYPE" != "Debug" ]; then
    echo "❌ Build type inválido: $BUILD_TYPE"
    echo ""
    echo "Uso: $0 [Release|Debug] [--rebuild] [SDL2] [--native|--compat]"
    echo "  Release    - Build otimizado para produção (padrão)"
    echo "  Debug      - Build com símbolos de debug"
    echo "  --rebuild  - Força rebuild completo da imagem Docker (mais lento)"
    echo "  SDL2       - Compilar com SDL2 (suporte DirectFB/framebuffer)"
    echo "  --native   - Build com -march=native (uso local — não distribuir)"
    echo "  --compat   - Build com baseline ARMv8-A portátil (padrão)"
    exit 1
fi

# Validar opção de compatibilidade
if [ "$BUILD_COMPATIBLE" != "ON" ] && [ "$BUILD_COMPATIBLE" != "OFF" ]; then
    echo "❌ BUILD_COMPATIBLE_ARM64 inválido: $BUILD_COMPATIBLE"
    echo "   Use: ON ou OFF"
    exit 1
fi

# Verificar se SDL2 foi solicitado e mostrar mensagem inicial
if [ "$BUILD_WITH_SDL2_ARG" = "SDL2" ] || [ "$BUILD_WITH_SDL2_ARG" = "sdl2" ] || \
   [ "$BUILD_WITH_SDL2_ARG" = "ON" ] || [ "$BUILD_WITH_SDL2_ARG" = "on" ]; then
    echo "🔧 Build com SDL2 habilitado (DirectFB/framebuffer)"
    echo ""
fi

echo "🐳 RetroCapture - Build para Linux ARM64 usando Docker"
echo "======================================================"
echo "📦 Build type: $BUILD_TYPE"
echo "🏗️  Arquitetura: ARM64 (aarch64) - Raspberry Pi 4/5"
echo "🔧 Base: arm64v8/debian:trixie (FFmpeg 6.1)"
echo "✅ Compatível com: Debian Trixie ARM64, Raspberry Pi OS 64-bit"
if [ "$BUILD_COMPATIBLE" = "ON" ]; then
    echo "🔧 Modo compatível: ON (baseline ARMv8-A, sem SVE/crypto — recomendado para distribuição)"
else
    echo "⚡ Modo compatível: OFF (-march=native — só roda em CPUs equivalentes à do build host)"
fi
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

# Verificar e configurar Docker Buildx para multiplataforma
echo "🔧 Configurando Docker Buildx para cross-compilation..."
if ! $DOCKER_CMD buildx version &>/dev/null; then
    echo "❌ Docker Buildx não está disponível!"
    echo ""
    echo "   Buildx é necessário para fazer cross-compilation ARM em sistemas x86_64"
    echo ""
    echo "   📦 Para instalar o Docker Buildx:"
    echo ""
    echo "   Ubuntu/Debian:"
    echo "     sudo apt-get update"
    echo "     sudo apt-get install -y docker-buildx"
    echo ""
    echo "   Após instalar, reinicie o Docker:"
    echo "     sudo systemctl restart docker"
    echo ""
    exit 1
fi

# Verificar versão do Buildx
BUILDX_VERSION=$($DOCKER_CMD buildx version 2>/dev/null | head -1)
echo "   ✅ Docker Buildx encontrado: $BUILDX_VERSION"

# Verificar se QEMU está disponível para emulação ARM
QEMU_CONFIGURED=false
if [ -f /proc/sys/fs/binfmt_misc/qemu-aarch64 ]; then
    QEMU_CONFIGURED=true
    echo "   ✅ QEMU para ARM64 está configurado"
elif command -v qemu-aarch64-static &>/dev/null || command -v qemu-aarch64 &>/dev/null; then
    QEMU_CONFIGURED=true
    echo "   ✅ QEMU para ARM64 está disponível"
else
    echo "⚠️  QEMU para ARM64 não está configurado no sistema"
    echo "   Tentando configurar automaticamente..."
    if command -v apt-get &>/dev/null; then
        if sudo apt-get install -y qemu-user-static binfmt-support 2>/dev/null; then
            QEMU_CONFIGURED=true
            echo "   ✅ QEMU instalado com sucesso"
        else
            echo "   ⚠️  Falha ao instalar QEMU automaticamente"
            echo "   Execute manualmente: sudo apt-get install -y qemu-user-static binfmt-support"
            echo "   Ou: sudo docker run --rm --privileged multiarch/qemu-user-static --reset -p yes"
        fi
    else
        echo "   Execute: sudo docker run --rm --privileged multiarch/qemu-user-static --reset -p yes"
    fi
    echo ""
fi

# Criar builder multiplataforma se não existir
BUILDER_NAME="retrocapture-arm64v8-builder"
echo "   📦 Configurando builder multiplataforma..."

# Verificar se o builder já existe
if $DOCKER_CMD buildx ls 2>/dev/null | grep -q "$BUILDER_NAME"; then
    echo "   ✅ Builder '$BUILDER_NAME' encontrado, usando..."
    $DOCKER_CMD buildx use "$BUILDER_NAME" 2>/dev/null || {
        echo "   ⚠️  Erro ao usar builder, tentando criar novo..."
        $DOCKER_CMD buildx rm "$BUILDER_NAME" 2>/dev/null || true
    }
fi

# Se não existe, criar novo builder
if ! $DOCKER_CMD buildx ls 2>/dev/null | grep -q "$BUILDER_NAME"; then
    echo "   📦 Criando novo builder multiplataforma..."
    if $DOCKER_CMD buildx create --name "$BUILDER_NAME" --driver docker-container --use --bootstrap 2>/dev/null; then
        echo "   ✅ Builder criado com sucesso!"
    else
        echo "   ⚠️  Erro ao criar builder, usando builder padrão..."
        $DOCKER_CMD buildx use default 2>/dev/null || {
            echo "   ⚠️  Usando builder padrão do sistema..."
        }
    fi
fi

# Verificar builder atual
CURRENT_BUILDER=$($DOCKER_CMD buildx ls 2>/dev/null | grep '*' | awk '{print $1}' || echo "default")
echo "   ✅ Builder ativo: $CURRENT_BUILDER"

echo ""
echo "📦 Construindo imagem Docker para ARM64..."
if [ -n "$FORCE_REBUILD" ]; then
    echo "   🔄 Forçando rebuild completo (sem cache)..."
    echo "   Isso pode demorar vários minutos..."
    BUILD_FLAGS="--no-cache"
else
    echo "   Isso pode demorar alguns minutos na primeira vez..."
fi
echo "   (Usando arm64v8/debian:trixie - FFmpeg 6.1)"
echo "   (Cross-compilation via QEMU em sistema x86_64)"
echo ""

# Usar docker buildx build diretamente com plataforma específica
IMAGE_NAME="retrocapture-linux-arm64v8-builder"
IMAGE_TAG="$IMAGE_NAME:latest"

# Construir a imagem usando buildx com --load
# Nota: --load requer QEMU configurado via binfmt_misc
BUILD_ARGS="--platform linux/arm64 --file Dockerfile.linux-arm64v8 --tag $IMAGE_TAG --load"
if [ -n "$FORCE_REBUILD" ]; then
    BUILD_ARGS="$BUILD_ARGS --no-cache"
fi

if ! $DOCKER_CMD buildx build $BUILD_ARGS . > build-linux-arm64v8-image.log 2>&1; then
    echo "❌ Falha ao construir imagem Docker!"
    echo "   Verifique build-linux-arm64v8-image.log para detalhes"
    echo ""
    echo "💡 Solução: Configure QEMU para emulação ARM64 executando:"
    echo "   sudo docker run --rm --privileged multiarch/qemu-user-static --reset -p yes"
    echo ""
    echo "   Ou instale manualmente:"
    echo "   sudo apt-get install -y qemu-user-static binfmt-support"
    echo "   sudo systemctl restart docker"
    exit 1
fi

echo ""
echo "✅ Imagem construída com sucesso!"
echo "🔨 Compilando RetroCapture no container ARM64..."
echo ""

# Executar o container usando a imagem construída
# BUILD_WITH_SDL2 já foi processado acima
if [ "$BUILD_WITH_SDL2_ARG" = "SDL2" ] || [ "$BUILD_WITH_SDL2_ARG" = "sdl2" ] || \
   [ "$BUILD_WITH_SDL2_ARG" = "ON" ] || [ "$BUILD_WITH_SDL2_ARG" = "on" ]; then
    echo "   🔧 Build com SDL2 habilitado (DirectFB/framebuffer)"
    BUILD_WITH_SDL2="ON"
else
    BUILD_WITH_SDL2="OFF"
fi

# Executar o container com emulação ARM64
# Se QEMU não estiver configurado, tentar usar --privileged para permitir emulação
if [ "$QEMU_CONFIGURED" != "true" ]; then
    echo "⚠️  Executando com --privileged (QEMU pode não estar configurado corretamente)"
    DOCKER_RUN_ARGS="--privileged"
else
    DOCKER_RUN_ARGS=""
fi

if ! $DOCKER_CMD run --rm $DOCKER_RUN_ARGS \
    --platform linux/arm64 \
    -e BUILD_TYPE="$BUILD_TYPE" \
    -e BUILD_WITH_SDL2="$BUILD_WITH_SDL2" \
    -e BUILD_COMPATIBLE_ARM64="$BUILD_COMPATIBLE" \
    -v "$(pwd):/work:ro" \
    -v "$(pwd)/build-linux-arm64v8:/work/build-linux-arm64v8:rw" \
    -w /work \
    "$IMAGE_TAG" > build-linux-arm64v8.log 2>&1; then
    echo "❌ Falha na compilação!"
    echo "   Verifique build-linux-arm64v8.log para detalhes"
    echo ""
    if grep -q "exec format error" build-linux-arm64v8.log 2>/dev/null; then
        echo "💡 Erro detectado: 'exec format error'"
        echo "   Isso indica que o QEMU não está configurado corretamente para emular ARM64"
        echo ""
        echo "   Para corrigir, execute:"
        echo "   sudo docker run --rm --privileged multiarch/qemu-user-static --reset -p yes"
        echo ""
        echo "   Ou instale manualmente:"
        echo "   sudo apt-get install -y qemu-user-static binfmt-support"
        echo "   sudo systemctl restart docker"
    fi
    exit 1
fi

echo ""
echo "✅ Concluído!"
echo "📁 Executável: ./build-linux-arm64v8/bin/retrocapture"
echo ""
if [ "$BUILD_WITH_SDL2" = "ON" ]; then
    echo "💡 Este binário foi compilado com SDL2 (suporte DirectFB/framebuffer)"
    echo "   Para usar DirectFB: export SDL_VIDEODRIVER=directfb && ./build-linux-arm64v8/bin/retrocapture"
    echo "   Para usar framebuffer: export SDL_VIDEODRIVER=fbcon && ./build-linux-arm64v8/bin/retrocapture"
else
    echo "ℹ️  Nota: Este executável foi compilado para ARM64 (aarch64)"
    echo "   Compatível com Raspberry Pi 4/5 e outros sistemas ARM64"
fi
