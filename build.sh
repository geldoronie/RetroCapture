#!/bin/bash

# Script de build para RetroCapture

set -e

echo "=== RetroCapture Build Script ==="

# Criar diretório de build
mkdir -p build
cd build

# Configurar CMake
echo "Configurando CMake..."
cmake ..

# Compilar
echo "Compilando..."
make -j$(nproc)

echo ""
echo "=== Build concluído! ==="
echo "Executável: build/bin/retrocapture"
echo ""
echo "Para executar:"
echo "  ./build/bin/retrocapture"

