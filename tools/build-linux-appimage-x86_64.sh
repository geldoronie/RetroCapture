#!/bin/bash

# Script para gerar AppImage do RetroCapture

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

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== RetroCapture AppImage Builder ==="
echo "📦 Build type: $BUILD_TYPE"
echo ""

# Verificar se estamos no diretório correto
if [ ! -f "CMakeLists.txt" ]; then
    echo "Erro: Execute este script a partir do diretório raiz do projeto"
    exit 1
fi

# Versão da aplicação (obtida do CMakeLists.txt)
# Suporta versões com sufixos como -alpha, -beta, etc.
VERSION=$(grep -E "^project\(RetroCapture VERSION" CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+[^ ]*).*/\1/' || echo "0.5.0-alpha")
# Sufixo -alpha pra alinhar com convenção das tags (CMake VERSION é só numérica)
RELEASE_VERSION="${VERSION}-alpha"
APP_NAME="RetroCapture"
APP_ID="com.retrocapture.app"
APP_DIR="AppDir"
APPIMAGE_NAME="${APP_NAME}-${VERSION}-x86_64.AppImage"
RELEASE_APPIMAGE="${APP_NAME}-${RELEASE_VERSION}-linux-x86_64.AppImage"

# Limpar builds anteriores e artefatos
echo "Limpando builds anteriores e artefatos..."
rm -rf "$APP_DIR"
rm -f "$APPIMAGE_NAME"
# Limpar AppImages geradas anteriormente (com diferentes nomes possíveis)
rm -f "${APP_NAME}"-*.AppImage
rm -f "RetroCapture"*.AppImage
# Limpar arquivos temporários do appimagetool se foram baixados
if [ -f "appimagetool-x86_64.AppImage" ] && [ ! -f ".keep-appimagetool" ]; then
    echo "Removendo appimagetool baixado..."
    rm -f appimagetool-x86_64.AppImage
fi
# Limpar arquivos temporários do linuxdeploy se foram baixados
if [ -f "linuxdeploy-x86_64.AppImage" ] && [ ! -f ".keep-linuxdeploy" ]; then
    echo "Removendo linuxdeploy baixado..."
    rm -f linuxdeploy-x86_64.AppImage
fi
# Limpar diretório de plugins se existir
if [ -d "linuxdeploy-plugins" ]; then
    rm -rf linuxdeploy-plugins
fi

# Compilar a aplicação usando Docker
echo ""
echo "=== Compilando aplicação usando Docker ==="
if ! command -v docker &> /dev/null; then
    echo "Erro: Docker não está instalado. É necessário para compilar a aplicação."
    exit 1
fi

DOCKER_COMPOSE="docker-compose"
if ! command -v docker-compose &> /dev/null && docker compose version &> /dev/null; then
    DOCKER_COMPOSE="docker compose"
fi

echo "Construindo imagem Docker (se necessário)..."
$DOCKER_COMPOSE build build-linux-x86_64 > /dev/null 2>&1 || $DOCKER_COMPOSE build build-linux-x86_64

echo "Compilando RetroCapture no container Docker..."
$DOCKER_COMPOSE run --rm -e BUILD_TYPE="$BUILD_TYPE" build-linux-x86_64 > build-linux-x86_64.log 2>&1

if [ $? -ne 0 ]; then
    echo "Erro: Falha na compilação. Verifique build-linux-x86_64.log para mais detalhes."
    exit 1
fi

echo "Compilação concluída!"

# Gerar AppImage dentro do container Docker
echo ""
echo "=== Gerando AppImage dentro do container Docker ==="

# Verificar se o script auxiliar existe
if [ ! -f "tools/docker-build-appimage.sh" ]; then
    echo "❌ Erro: tools/docker-build-appimage.sh não encontrado"
    exit 1
fi

# Tornar o script executável
chmod +x tools/docker-build-appimage.sh

# Executar script de build do AppImage dentro do container
echo "Executando build do AppImage no container..."
echo "📋 Script: tools/docker-build-appimage.sh"
echo "📋 Versão: $VERSION"
echo "📋 Build type: $BUILD_TYPE"
echo ""

# O docker-compose mapeia .:/work:ro (read-only), então geramos o AppImage em /tmp
# e depois copiamos para o diretório de saída que tem permissão de escrita
# IMPORTANTE: Usar --entrypoint para sobrescrever o entrypoint padrão do container
# O entrypoint padrão executa o script de build, então precisamos sobrescrever
$DOCKER_COMPOSE run --rm \
    --entrypoint /bin/bash \
    -e BUILD_TYPE="$BUILD_TYPE" \
    -e VERSION="$VERSION" \
    -v "$(pwd):/work-output:rw" \
    build-linux-x86_64 \
    -c "set -e; echo '=== Iniciando build do AppImage ==='; cd /work; echo '📋 Diretório atual: \$(pwd)'; echo '📋 Verificando se script existe...'; if [ ! -f tools/docker-build-appimage.sh ]; then echo '❌ Erro: tools/docker-build-appimage.sh não encontrado!'; echo '📋 Conteúdo de /work/tools:'; ls -la /work/tools/ | head -20; exit 1; fi; echo '✅ Script encontrado'; echo '📋 Executando script...'; OUTPUT_DIR=/work-output bash tools/docker-build-appimage.sh" > build-appimage.log 2>&1

EXIT_CODE=$?

# Mostrar últimas linhas do log em caso de erro
if [ $EXIT_CODE -ne 0 ] || [ ! -f "$APPIMAGE_NAME" ]; then
    echo "❌ Erro: Falha na geração do AppImage"
    echo ""
    echo "📋 Últimas 30 linhas do log:"
    echo "----------------------------------------"
    tail -30 build-appimage.log || echo "(log vazio ou não encontrado)"
    echo "----------------------------------------"
    echo ""
    echo "Verifique build-appimage.log para mais detalhes"
    exit 1
fi

# Verificar se o AppImage foi gerado
if [ -f "$APPIMAGE_NAME" ]; then
    # Mover pra dist/ com nome de release padronizado (linux-x86_64 + sufixo -alpha)
    mkdir -p dist
    mv "$APPIMAGE_NAME" "dist/$RELEASE_APPIMAGE"
    echo ""
    echo "✅ === AppImage gerada com sucesso! ==="
    echo "📦 Arquivo: dist/$RELEASE_APPIMAGE"
    echo "📏 Tamanho: $(du -h "dist/$RELEASE_APPIMAGE" | cut -f1)"
    echo "📍 Localização: $(pwd)/dist/$RELEASE_APPIMAGE"
    echo ""
    echo "Para executar:"
    echo "  chmod +x dist/$RELEASE_APPIMAGE"
    echo "  ./dist/$RELEASE_APPIMAGE"
    exit 0
else
    echo "❌ Erro: AppImage não foi gerada"
    echo "Verifique build-appimage.log para mais detalhes"
    exit 1
fi

