#!/bin/bash

# Script de build para RetroCapture

set -e

# Determinar modo de build (debug ou release)
BUILD_TYPE="${1:-release}"

# Normalizar para minúsculas
BUILD_TYPE=$(echo "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')

# Validar modo
if [ "$BUILD_TYPE" != "debug" ] && [ "$BUILD_TYPE" != "release" ]; then
    echo "Uso: $0 [debug|release]"
    echo "  debug   - Build com símbolos de debug e otimização desabilitada"
    echo "  release - Build otimizado para produção (padrão)"
    exit 1
fi

# Converter para formato CMake
if [ "$BUILD_TYPE" = "debug" ]; then
    CMAKE_BUILD_TYPE="Debug"
else
    CMAKE_BUILD_TYPE="Release"
fi

echo "=== RetroCapture Build Script ==="
echo "Modo: $CMAKE_BUILD_TYPE"

# Criar diretório de build
mkdir -p build
cd build

# Configurar CMake com o tipo de build
echo "Configurando CMake ($CMAKE_BUILD_TYPE)..."
cmake -DCMAKE_BUILD_TYPE=$CMAKE_BUILD_TYPE ..

# Compilar
echo "Compilando..."
make clean
make -j$(nproc)

echo ""
echo "=== Build concluído! ==="
echo "Modo: $CMAKE_BUILD_TYPE"
echo "Executável: build/bin/retrocapture"
echo ""
echo "Para executar:"
echo "  ./build/bin/retrocapture"
echo ""
echo "Para rebuild em outro modo:"
echo "  ./build.sh debug    - Build com debug"
echo "  ./build.sh release  - Build otimizado"

