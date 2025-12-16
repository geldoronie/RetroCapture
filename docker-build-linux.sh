#!/bin/bash
set -e

# Build type: Release (default) or Debug
BUILD_TYPE="${BUILD_TYPE:-Release}"

# Validar build type
if [ "$BUILD_TYPE" != "Release" ] && [ "$BUILD_TYPE" != "Debug" ]; then
    echo "❌ Build type inválido: $BUILD_TYPE"
    echo "   Use: Release ou Debug"
    exit 1
fi

echo "🚀 Compilando RetroCapture para Linux..."
echo "📦 Build type: $BUILD_TYPE"
echo ""

# Verificar se estamos no diretório correto
if [ ! -f "CMakeLists.txt" ]; then
    echo "❌ CMakeLists.txt não encontrado!"
    exit 1
fi

# Configurar Git ANTES de qualquer operação (resolve "dubious ownership" no Docker)
# Isso deve ser feito antes de entrar no diretório de build
echo "⚙️  Configurando Git..."
git config --global --add safe.directory '*' || true

# Criar diretório de build (limpar cache CMake se existir)
BUILD_DIR="build-linux"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Limpar cache do CMake se existir (pode ter sido criado fora do container)
if [ -f "CMakeCache.txt" ]; then
    echo "🧹 Limpando cache do CMake..."
    rm -f CMakeCache.txt
    rm -rf CMakeFiles
fi

# Limpar diretório _deps se existir (pode ter sido criado com permissões incorretas)
# Isso garante que o FetchContent baixe tudo do zero com as permissões corretas
if [ -d "_deps" ]; then
    echo "🧹 Limpando dependências anteriores..."
    rm -rf _deps
fi

echo "⚙️  Configurando CMake..."
cmake .. \
-DCMAKE_BUILD_TYPE="$BUILD_TYPE"

echo ""
echo "🔨 Compilando..."
cmake --build . -j$(nproc)

echo ""
echo "✅ Build concluído!"
echo ""
echo "📁 Executável: $(pwd)/bin/retrocapture"
