#!/bin/bash
set -e

cd /work

# Verificar se build-windows existe
if [ ! -d "build-windows" ]; then
    echo "Erro: Diretório build-windows não encontrado em /work"
    exit 1
fi

cd build-windows

# Verificar se NSIS está disponível no container
if ! command -v makensis &> /dev/null; then
    echo "Erro: NSIS não está disponível no container Docker"
    echo "É necessário instalar NSIS no Dockerfile.windows"
    exit 1
fi

NSIS_PATH=$(which makensis)
echo "NSIS encontrado: ${NSIS_PATH}"
echo "Versão NSIS: $(makensis -VERSION 2>&1 || echo 'N/A')"

# Garantir que makensis está no PATH
export PATH="/usr/bin:/usr/local/bin:${PATH}"

# Verificar se CPack está configurado e reconfigurar se necessário para detectar NSIS
echo "Verificando configuração CPack..."
export PKG_CONFIG_PATH=/usr/src/mxe/usr/x86_64-w64-mingw32.static/lib/pkgconfig

# Sempre reconfigurar para garantir que CPack detecta o NSIS e usa a versão correta
# Limpar cache do CPack para forçar reconfiguração
MAKENSIS_PATH=$(which makensis)
echo "Reconfigurando CMake para detectar NSIS em ${MAKENSIS_PATH}..."
echo "Limpando cache do CPack para garantir versão correta..."
rm -f CPackConfig.cmake CPackSourceConfig.cmake
# Remover variáveis CPACK do cache do CMake
sed -i '/^CPACK_PACKAGE_FILE_NAME:/d' CMakeCache.txt 2>/dev/null || true
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=/usr/src/mxe/usr/x86_64-w64-mingw32.static/share/cmake/mxe-conf.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCPACK_NSIS_EXECUTABLE="${MAKENSIS_PATH}"

# Verificar se o executável existe
if [ ! -f "bin/retrocapture.exe" ]; then
    echo "Erro: retrocapture.exe não encontrado em bin/"
    exit 1
fi

# Configurar CPack para usar o caminho correto do NSIS
# CPack precisa encontrar makensis no PATH ou via variável de ambiente
export PATH="/usr/bin:${PATH}"

# Verificar se makensis está no PATH
if ! command -v makensis &> /dev/null; then
    echo "Erro: makensis não está no PATH"
    echo "PATH atual: ${PATH}"
    exit 1
fi

# Gerar instalador
# CPack precisa encontrar makensis - vamos garantir que está no PATH e configurado
MAKENSIS_PATH=$(which makensis)
echo ""
echo "Gerando instalador com CPack..."
echo "Usando makensis: ${MAKENSIS_PATH}"

# CPack pode precisar da variável de ambiente NSIS_MAKENSIS ou CPACK_NSIS_EXECUTABLE
# Vamos tentar definir explicitamente
export NSIS_MAKENSIS="${MAKENSIS_PATH}"

# Gerar instalador com verbose para debug
cpack -G NSIS -V -D CPACK_NSIS_EXECUTABLE="${MAKENSIS_PATH}" 2>&1 || \
cpack -G NSIS -V

if [ $? -eq 0 ]; then
    echo ""
    echo "Instalador gerado com sucesso!"
    echo "Arquivos .exe encontrados:"
    ls -lh *.exe 2>/dev/null || echo "Nenhum arquivo .exe encontrado"
    echo ""
    echo "Todos os arquivos no diretório:"
    ls -la | head -20
else
    echo "Erro ao gerar instalador (código: $?)"
    exit 1
fi
