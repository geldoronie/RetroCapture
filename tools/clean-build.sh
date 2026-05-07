#!/bin/bash
# Script para limpar completamente um build anterior
# Uso: ./tools/clean-build.sh [diretório-do-build]

set -e

BUILD_DIR="${1:-build-linux-x86_64}"

if [ ! -d "$BUILD_DIR" ]; then
    echo "⚠️  Diretório de build não encontrado: $BUILD_DIR"
    echo "   Nada para limpar."
    exit 0
fi

echo "🧹 Limpando build anterior: $BUILD_DIR"
echo ""

# Confirmar antes de deletar
read -p "Tem certeza que deseja deletar $BUILD_DIR? (s/N): " -n 1 -r
echo ""

if [[ ! $REPLY =~ ^[Ss]$ ]]; then
    echo "❌ Operação cancelada."
    exit 0
fi

echo "🗑️  Removendo $BUILD_DIR..."
rm -rf "$BUILD_DIR"

echo "✅ Build limpo com sucesso!"
echo ""
echo "💡 Agora você pode recompilar:"
echo "   mkdir -p $BUILD_DIR"
echo "   cd $BUILD_DIR"
echo "   cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_COMPATIBLE_X86_64=ON"
echo "   cmake --build . -j\$(nproc)"
