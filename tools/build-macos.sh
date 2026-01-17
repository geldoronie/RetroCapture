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

echo "🍎 RetroCapture - Build para macOS"
echo "=================================="
echo "📦 Build type: $BUILD_TYPE"
echo "🏗️  Arquitetura: $(uname -m)"
echo "🖥️  Sistema: $(sw_vers -productName) $(sw_vers -productVersion)"
echo ""

# Verificar se estamos no macOS
if [[ "$OSTYPE" != "darwin"* ]]; then
    echo "❌ Este script só funciona no macOS!"
    exit 1
fi

# Verificar dependências
echo "🔍 Verificando dependências..."
echo ""

MISSING_DEPS=()

if ! command -v cmake &> /dev/null; then
    MISSING_DEPS+=("cmake")
fi

if ! command -v brew &> /dev/null; then
    MISSING_DEPS+=("homebrew")
fi

if [ ${#MISSING_DEPS[@]} -gt 0 ]; then
    echo "❌ Dependências faltando:"
    for dep in "${MISSING_DEPS[@]}"; do
        echo "   - $dep"
    done
    echo ""
    echo "   Execute primeiro: ./tools/install-deps-macos.sh"
    exit 1
fi

# Verificar bibliotecas via pkg-config
echo "   Verificando bibliotecas..."
MISSING_LIBS=()

if ! pkg-config --exists glfw3 2>/dev/null; then
    MISSING_LIBS+=("glfw3")
fi

if ! pkg-config --exists libavcodec 2>/dev/null; then
    MISSING_LIBS+=("libavcodec (FFmpeg)")
fi

if ! pkg-config --exists libpng 2>/dev/null; then
    MISSING_LIBS+=("libpng")
fi

if [ ${#MISSING_LIBS[@]} -gt 0 ]; then
    echo "❌ Bibliotecas faltando:"
    for lib in "${MISSING_LIBS[@]}"; do
        echo "   - $lib"
    done
    echo ""
    echo "   Execute: ./tools/install-deps-macos.sh"
    exit 1
fi

echo "✅ Todas as dependências estão instaladas"
echo ""

# Diretório de build
BUILD_DIR="build-macos-$(uname -m)"
echo "📁 Diretório de build: $BUILD_DIR"
echo ""

# Criar diretório de build
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configurar CMake
echo "⚙️  Configurando CMake..."
echo ""

cmake .. \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0

if [ $? -ne 0 ]; then
    echo ""
    echo "❌ Falha na configuração do CMake!"
    exit 1
fi

echo ""
echo "🔨 Compilando RetroCapture..."
echo "   Isso pode demorar alguns minutos..."
echo ""

# Compilar
cmake --build . --config "$BUILD_TYPE" -j$(sysctl -n hw.ncpu)

if [ $? -ne 0 ]; then
    echo ""
    echo "❌ Falha na compilação!"
    exit 1
fi

echo ""
echo "✅ Build concluído com sucesso!"
echo ""
echo "📁 Executável: $BUILD_DIR/bin/retrocapture"
echo ""
echo "🚀 Para executar:"
echo "   ./$BUILD_DIR/bin/retrocapture"
echo ""
echo "📝 Exemplo de uso:"
echo "   ./$BUILD_DIR/bin/retrocapture --source avfoundation --width 1920 --height 1080"
echo ""
