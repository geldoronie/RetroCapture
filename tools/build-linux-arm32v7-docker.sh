#!/bin/bash
set -e

# Build type: Release (default) or Debug
BUILD_TYPE="${1:-Release}"
FORCE_REBUILD=""
BUILD_WITH_SDL2_ARG=""

# Processar argumentos
for arg in "$@"; do
    case "$arg" in
        Release|Debug)
            BUILD_TYPE="$arg"
            ;;
        --rebuild)
            FORCE_REBUILD="--rebuild"
            ;;
        SDL2|sdl2|ON|on)
            BUILD_WITH_SDL2_ARG="$arg"
            ;;
        *)
            if [ "$arg" != "$BUILD_TYPE" ]; then
                echo "⚠️  Argumento desconhecido: $arg (ignorado)"
            fi
            ;;
    esac
done

# Validar build type
if [ "$BUILD_TYPE" != "Release" ] && [ "$BUILD_TYPE" != "Debug" ]; then
    echo "❌ Build type inválido: $BUILD_TYPE"
    echo ""
    echo "Uso: $0 [Release|Debug] [--rebuild] [SDL2]"
    echo "  Release - Build otimizado para produção (padrão)"
    echo "  Debug   - Build com símbolos de debug"
    echo "  --rebuild - Força rebuild completo da imagem Docker (mais lento)"
    echo "  SDL2    - Compilar com SDL2 (suporte DirectFB/framebuffer)"
    exit 1
fi

# Verificar se SDL2 foi solicitado e mostrar mensagem inicial
if [ "$BUILD_WITH_SDL2_ARG" = "SDL2" ] || [ "$BUILD_WITH_SDL2_ARG" = "sdl2" ] || \
   [ "$BUILD_WITH_SDL2_ARG" = "ON" ] || [ "$BUILD_WITH_SDL2_ARG" = "on" ]; then
    echo "🔧 Build com SDL2 habilitado (DirectFB/framebuffer)"
    echo ""
fi

echo "🐳 RetroCapture - Build para Linux ARM32v7 usando Docker"
echo "======================================================"
echo "📦 Build type: $BUILD_TYPE"
echo "🏗️  Arquitetura: ARM32v7 (armhf) - Raspberry Pi 3"
echo "🔧 Base: arm32v7/debian:trixie (FFmpeg 6.1)"
echo "✅ Compatível com: Debian Trixie, Raspberry Pi OS (Debian Trixie)"
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
    echo "   Ou se você tem Docker Desktop/versão mais recente:"
    echo "     O Buildx já deve estar incluído. Verifique com:"
    echo "     docker buildx version"
    echo ""
    echo "   Após instalar, reinicie o Docker:"
    echo "     sudo systemctl restart docker"
    echo ""
    echo "   💡 Alternativa: Se você tem acesso a uma Raspberry Pi 3,"
    echo "      pode fazer o build diretamente nela (muito mais rápido!)"
    echo ""
    exit 1
fi

# Verificar versão do Buildx
BUILDX_VERSION=$($DOCKER_CMD buildx version 2>/dev/null | head -1)
echo "   ✅ Docker Buildx encontrado: $BUILDX_VERSION"

# Verificar se QEMU está disponível para emulação ARM
if [ ! -f /proc/sys/fs/binfmt_misc/qemu-arm ]; then
    echo "⚠️  QEMU para ARM não está configurado no sistema"
    echo "   Tentando instalar qemu-user-static..."
    if command -v apt-get &>/dev/null; then
        echo "   Execute: sudo apt-get install -y qemu-user-static binfmt-support"
        echo "   Ou: sudo docker run --rm --privileged multiarch/qemu-user-static --reset -p yes"
    fi
    echo ""
fi

# Criar builder multiplataforma se não existir
BUILDER_NAME="retrocapture-arm32v7-builder"
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
echo "📦 Construindo imagem Docker para ARM32v7..."
if [ -n "$FORCE_REBUILD" ]; then
    echo "   🔄 Forçando rebuild completo (sem cache)..."
    echo "   Isso pode demorar vários minutos..."
    BUILD_FLAGS="--no-cache"
else
    echo "   Isso pode demorar alguns minutos na primeira vez..."
fi
echo "   (Usando arm32v7/debian:trixie - FFmpeg 6.1)"
echo "   (Cross-compilation via QEMU em sistema x86_64)"
echo ""

# Usar docker buildx build diretamente com plataforma específica
IMAGE_NAME="retrocapture-linux-arm32v7-builder"
IMAGE_TAG="$IMAGE_NAME:latest"
echo "   (Isso pode demorar bastante na primeira vez devido à emulação QEMU)"
echo ""

# Construir a imagem usando buildx com --load
# Nota: --load requer QEMU configurado via binfmt_misc
BUILD_ARGS="--platform linux/arm/v7 --file Dockerfile.linux-arm32v7 --tag $IMAGE_TAG --load"
if [ -n "$FORCE_REBUILD" ]; then
    BUILD_ARGS="$BUILD_ARGS --no-cache"
fi

if ! $DOCKER_CMD buildx build $BUILD_ARGS . > build-linux-arm32v7-image.log 2>&1; then
    echo "❌ Falha ao construir imagem Docker!"
    echo "   Verifique build-linux-arm32v7-image.log para detalhes"
    echo ""
    echo "💡 Solução: Configure QEMU para emulação ARM executando:"
    echo "   sudo docker run --rm --privileged multiarch/qemu-user-static --reset -p yes"
    echo ""
    echo "   Ou instale manualmente:"
    echo "   sudo apt-get install -y qemu-user-static binfmt-support"
    echo "   sudo systemctl restart docker"
    exit 1
fi

echo ""
echo "✅ Imagem construída com sucesso!"
echo "🔨 Compilando RetroCapture no container ARM32v7..."
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

$DOCKER_CMD run --rm \
    --platform linux/arm/v7 \
    -e BUILD_TYPE="$BUILD_TYPE" \
    -e BUILD_WITH_SDL2="$BUILD_WITH_SDL2" \
    -v "$(pwd):/work:ro" \
    -v "$(pwd)/build-linux-arm32v7:/work/build-linux-arm32v7:rw" \
    -w /work \
    "$IMAGE_TAG" > build-linux-arm32v7.log 2>&1

if [ $? -ne 0 ]; then
    echo "❌ Falha na compilação!"
    echo "   Verifique build-linux-arm32v7.log para detalhes"
    exit 1
fi

echo ""
echo "✅ Concluído!"
echo "📁 Executável: ./build-linux-arm32v7/bin/retrocapture"
echo ""

# Empacotar tar.gz pós-build em dist/
VERSION=$(grep -E "^project\(RetroCapture VERSION" CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+[^ ]*).*/\1/' || echo "unknown")
RELEASE_VERSION="${VERSION}-alpha"
SDL_SUFFIX=""
if [ "$BUILD_WITH_SDL2" = "ON" ]; then
    SDL_SUFFIX="-sdl2"
fi
TARBALL_NAME="RetroCapture-${RELEASE_VERSION}-linux-arm32v7${SDL_SUFFIX}.tar.gz"
STAGING_DIR="dist/staging-arm32v7"
mkdir -p dist
rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR/RetroCapture-${RELEASE_VERSION}-linux-arm32v7${SDL_SUFFIX}"

if [ -d "build-linux-arm32v7/bin" ]; then
    cp -r build-linux-arm32v7/bin/. "$STAGING_DIR/RetroCapture-${RELEASE_VERSION}-linux-arm32v7${SDL_SUFFIX}/"
    if [ -f "README.md" ]; then
        cp README.md "$STAGING_DIR/RetroCapture-${RELEASE_VERSION}-linux-arm32v7${SDL_SUFFIX}/" 2>/dev/null || true
    fi
    if [ -f "LICENSE" ]; then
        cp LICENSE "$STAGING_DIR/RetroCapture-${RELEASE_VERSION}-linux-arm32v7${SDL_SUFFIX}/" 2>/dev/null || true
    fi
    rm -f "dist/$TARBALL_NAME"
    tar -C "$STAGING_DIR" -czf "dist/$TARBALL_NAME" "RetroCapture-${RELEASE_VERSION}-linux-arm32v7${SDL_SUFFIX}"
    rm -rf "$STAGING_DIR"
    echo "📦 Tarball: $(pwd)/dist/$TARBALL_NAME"
    echo "📏 Tamanho: $(du -h dist/$TARBALL_NAME | cut -f1)"
    echo ""
fi

if [ "$BUILD_WITH_SDL2" = "ON" ]; then
    echo "💡 Este binário foi compilado com SDL2 (suporte DirectFB/framebuffer)"
    echo "   Para usar DirectFB: export SDL_VIDEODRIVER=directfb && ./build-linux-arm32v7/bin/retrocapture"
    echo "   Para usar framebuffer: export SDL_VIDEODRIVER=fbcon && ./build-linux-arm32v7/bin/retrocapture"
else
    echo "ℹ️  Nota: Este executável foi compilado para ARM32v7 (Raspberry Pi 3)"
    echo "   Para executar, transfira para sua Raspberry Pi 3"
fi
