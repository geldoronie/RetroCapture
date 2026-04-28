#!/bin/bash
set -e

# Build type: Release (default) or Debug
BUILD_TYPE="${BUILD_TYPE:-Release}"

# Compatibilidade: builds dentro do Docker são pra distribuição, então default ON
# (baseline x86-64-v2, sem AVX/AVX2). Para -march=native passe BUILD_COMPATIBLE_X86_64=OFF.
BUILD_COMPATIBLE="${BUILD_COMPATIBLE_X86_64:-ON}"

# Validar build type
if [ "$BUILD_TYPE" != "Release" ] && [ "$BUILD_TYPE" != "Debug" ]; then
    echo "❌ Build type inválido: $BUILD_TYPE"
    echo "   Use: Release ou Debug"
    exit 1
fi

# Validar opção de compatibilidade
if [ "$BUILD_COMPATIBLE" != "ON" ] && [ "$BUILD_COMPATIBLE" != "OFF" ]; then
    echo "❌ BUILD_COMPATIBLE_X86_64 inválido: $BUILD_COMPATIBLE"
    echo "   Use: ON ou OFF"
    exit 1
fi

echo "🚀 Compilando RetroCapture para Linux x86_64..."
echo "📦 Build type: $BUILD_TYPE"
echo "🏗️  Arquitetura: x86_64 (amd64)"
if [ "$BUILD_COMPATIBLE" = "ON" ]; then
    echo "🔧 Modo compatível: ON (baseline x86-64-v2, sem AVX/AVX2 — recomendado para distribuição)"
else
    echo "⚡ Modo compatível: OFF (-march=native — só roda em CPUs equivalentes à do build host)"
fi
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
BUILD_DIR="build-linux-x86_64"
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
if [ "$BUILD_COMPATIBLE" = "ON" ]; then
    cmake .. \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DBUILD_COMPATIBLE_X86_64=ON
else
    cmake .. \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
fi

echo ""
echo "🔨 Compilando..."
cmake --build . -j$(nproc)

echo ""
echo "✅ Build concluído!"
echo ""
echo "📁 Executável: $(pwd)/bin/retrocapture"
