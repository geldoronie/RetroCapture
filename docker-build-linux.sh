#!/bin/bash
set -e

echo "🚀 Compilando RetroCapture para Linux..."
echo ""

# Verificar se estamos no diretório correto
if [ ! -f "CMakeLists.txt" ]; then
    echo "❌ CMakeLists.txt não encontrado!"
    exit 1
fi

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

echo "⚙️  Configurando CMake..."
cmake .. \
-DCMAKE_BUILD_TYPE=Release

echo ""
echo "🔨 Compilando..."
cmake --build . -j$(nproc)

echo ""
echo "✅ Build concluído!"
echo ""
echo "📁 Executável: $(pwd)/bin/retrocapture"
