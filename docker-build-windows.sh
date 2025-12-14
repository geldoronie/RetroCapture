#!/bin/bash
# Script de build dentro do container Docker

set -e

echo "🚀 RetroCapture - Build para Windows no Docker"
echo "=============================================="
echo ""

# Compilar dependências do MXE se necessário (primeira execução)
echo "📦 Verificando dependências do MXE..."
/usr/local/bin/docker-compile-deps.sh
echo ""

# Verificar se estamos no diretório correto
if [ ! -f "CMakeLists.txt" ]; then
    echo "❌ Erro: CMakeLists.txt não encontrado"
    echo "   Execute: docker-compose run --rm build-windows"
    exit 1
fi

# Criar diretório de build
BUILD_DIR="build-windows"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "📦 Configurando CMake..."
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE="${CMAKE_TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CROSSCOMPILING=ON

echo ""
echo "🔨 Compilando..."
cmake --build . -j$(nproc)

echo ""
echo "✅ Build concluído!"
echo ""
echo "📁 Arquivos gerados:"
find . -name "*.exe" -o -name "*.dll" | head -10

echo ""
echo "💡 Para copiar os arquivos para o host:"
echo "   docker cp <container-id>:/workspace/$BUILD_DIR/bin ./build-windows-bin"

