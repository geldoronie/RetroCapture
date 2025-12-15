#!/bin/bash
# Script para gerar instalador Windows do RetroCapture usando CPack

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== RetroCapture Windows Installer Builder ==="
echo ""

# Verificar se estamos no diretório correto
if [ ! -f "CMakeLists.txt" ]; then
    echo "Erro: Execute este script a partir do diretório raiz do projeto"
    exit 1
fi

# Versão da aplicação (obtida do CMakeLists.txt)
# O CPack vai gerar o nome automaticamente baseado em CPACK_PACKAGE_FILE_NAME
# Formato esperado: RetroCapture-{VERSION}-Windows-Setup.exe
VERSION=$(grep -E "^project\(RetroCapture VERSION" CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+[^ ]*).*/\1/' || echo "0.3.0")
INSTALLER_NAME="RetroCapture-${VERSION}-Windows-Setup.exe"

# Verificar se Docker está disponível
if ! command -v docker &> /dev/null; then
    echo "Erro: Docker não está instalado. É necessário para compilar a aplicação."
    exit 1
fi

DOCKER_COMPOSE="docker-compose"
if ! command -v docker-compose &> /dev/null && docker compose version &> /dev/null; then
    DOCKER_COMPOSE="docker compose"
fi

# Limpar instaladores anteriores
echo "Limpando instaladores anteriores..."
rm -f RetroCapture-*-Windows-Setup.exe
rm -f RetroCapture-*-Windows-Setup.msi
rm -f RetroCapture-*-win64.exe  # Remover formato antigo também

# Compilar a aplicação usando Docker
echo ""
echo "=== Compilando aplicação usando Docker ==="
echo "Construindo imagem Docker (se necessário)..."
$DOCKER_COMPOSE build build-windows > /dev/null 2>&1 || $DOCKER_COMPOSE build build-windows

echo "Compilando RetroCapture no container Docker..."
$DOCKER_COMPOSE run --rm build-windows > build-windows.log 2>&1

if [ $? -ne 0 ]; then
    echo "Erro: Falha na compilação. Verifique build-windows.log para mais detalhes."
    exit 1
fi

echo "Compilação concluída!"

# Verificar se o build foi bem-sucedido
if [ ! -d "build-windows" ]; then
    echo "Erro: Diretório build-windows não encontrado"
    exit 1
fi

if [ ! -f "build-windows/bin/retrocapture.exe" ]; then
    echo "Erro: Executável não encontrado em build-windows/bin/retrocapture.exe"
    echo "A compilação via Docker pode ter falhado. Verifique build-windows.log"
    exit 1
fi

# Verificar se NSIS está disponível no sistema (para gerar instalador localmente)
# Se não estiver, vamos tentar gerar dentro do container Docker
echo ""
echo "=== Gerando instalador Windows ==="

if command -v makensis &> /dev/null; then
    echo "NSIS encontrado no sistema, gerando instalador localmente..."
    cd build-windows
    cpack -G NSIS
    cd ..
    
    # Verificar se o instalador foi gerado
    if [ -f "build-windows/${INSTALLER_NAME}" ]; then
        # Mover para o diretório raiz
        mv "build-windows/${INSTALLER_NAME}" .
        echo ""
        echo "=== Instalador gerado com sucesso! ==="
        echo "Arquivo: ${INSTALLER_NAME}"
        echo "Tamanho: $(du -h "${INSTALLER_NAME}" | cut -f1)"
        exit 0
    else
        echo "Aviso: Instalador não encontrado em build-windows/${INSTALLER_NAME}"
        echo "Tentando gerar dentro do container Docker..."
    fi
fi

# Se NSIS não estiver disponível localmente, gerar dentro do container
echo "NSIS não encontrado localmente. Gerando instalador dentro do container Docker..."

# Criar script temporário para gerar instalador no container
# O script precisa estar no diretório do projeto para ter acesso ao build-windows
cat > build-installer-temp.sh << 'EOF'
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
EOF

chmod +x build-installer-temp.sh

# Executar script no container
# Usar --entrypoint para sobrescrever o ENTRYPOINT e executar apenas nosso script
# O docker-compose.yml já monta o diretório atual em /work
echo "Executando geração de instalador no container..."
$DOCKER_COMPOSE run --rm --entrypoint bash build-windows /work/build-installer-temp.sh

# Procurar pelo instalador com o nome esperado primeiro
INSTALLER_FOUND=""
# Priorizar o nome esperado: RetroCapture-{VERSION}-Windows-Setup.exe
if [ -f "build-windows/${INSTALLER_NAME}" ]; then
    INSTALLER_FOUND="build-windows/${INSTALLER_NAME}"
    echo "Instalador encontrado com nome esperado: ${INSTALLER_NAME}"
else
    # Se não encontrar, procurar por qualquer instalador (exceto o executável e formato antigo)
    # Ordenar por data de modificação (mais recente primeiro) e ignorar formato antigo
    for installer in $(ls -t build-windows/RetroCapture-*-Windows-Setup.exe build-windows/RetroCapture-*.exe 2>/dev/null | grep -v "win64.exe" | grep -v "retrocapture.exe"); do
        if [ -f "$installer" ] && [[ "$installer" == *"Windows-Setup.exe" ]] || [[ "$installer" == *"-Setup.exe" ]]; then
            INSTALLER_FOUND="$installer"
            echo "Instalador encontrado: $(basename "$installer")"
            break
        fi
    done
fi

# Se encontrou, mover para o diretório raiz
if [ -n "$INSTALLER_FOUND" ]; then
    INSTALLER_BASENAME=$(basename "$INSTALLER_FOUND")
    mv "$INSTALLER_FOUND" .
    echo ""
    echo "=== Instalador gerado com sucesso! ==="
    echo "Arquivo: ${INSTALLER_BASENAME}"
    echo "Tamanho: $(du -h "${INSTALLER_BASENAME}" | cut -f1)"
    rm -f build-installer-temp.sh
    exit 0
else
    echo ""
    echo "=== Aviso: Instalador não foi gerado ==="
    echo ""
    echo "Verificando build-windows/..."
    ls -la build-windows/*.exe 2>/dev/null || echo "Nenhum .exe encontrado em build-windows/"
    echo ""
    echo "Possíveis causas:"
    echo "  1. NSIS não está instalado no container Docker (rebuild necessário)"
    echo "  2. CPack não está configurado corretamente no CMakeLists.txt"
    echo "  3. Erro durante a geração do instalador (verifique logs acima)"
    echo ""
    echo "Para resolver:"
    echo "  1. Reconstrua a imagem Docker: docker-compose build build-windows"
    echo "  2. Verifique se CPack está ativo: grep CPack build-windows/CMakeCache.txt"
    echo "  3. Tente gerar manualmente: cd build-windows && cpack -G NSIS -V"
    # Limpar script temporário
    rm -f build-installer-temp.sh
    exit 1
fi

# Limpar script temporário após sucesso
rm -f build-installer-temp.sh
