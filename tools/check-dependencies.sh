#!/bin/bash
# Script para verificar dependências do RetroCapture usando ldd
# Uso: ./tools/check-dependencies.sh [caminho-do-executável]

set -e

# Cores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Função para detectar distribuição
detect_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        DISTRO_ID="${ID:-unknown}"
        DISTRO_NAME="${PRETTY_NAME:-$NAME}"
    else
        DISTRO_ID="unknown"
        DISTRO_NAME="Unknown"
    fi
}

# Função para sugerir comando de instalação baseado na distribuição
suggest_install_command() {
    local lib_name="$1"
    local missing_lib="$2"
    
    case "$DISTRO_ID" in
        arch|manjaro|endeavouros)
            # Tentar mapear biblioteca para pacote do Arch/Manjaro
            case "$missing_lib" in
                libglfw.so*)
                    echo "sudo pacman -S glfw"
                    ;;
                libSDL2.so*)
                    echo "sudo pacman -S sdl2"
                    ;;
                libpng*.so*)
                    echo "sudo pacman -S libpng"
                    ;;
                libavcodec.so*)
                    echo "sudo pacman -S ffmpeg"
                    ;;
                libavformat.so*)
                    echo "sudo pacman -S ffmpeg"
                    ;;
                libavutil.so*)
                    echo "sudo pacman -S ffmpeg"
                    ;;
                libswscale.so*)
                    echo "sudo pacman -S ffmpeg"
                    ;;
                libswresample.so*)
                    echo "sudo pacman -S ffmpeg"
                    ;;
                libv4l2.so*)
                    echo "sudo pacman -S v4l-utils"
                    ;;
                libpulse.so*)
                    echo "sudo pacman -S libpulse"
                    ;;
                libssl.so*|libcrypto.so*)
                    echo "sudo pacman -S openssl"
                    ;;
                libGL.so*)
                    echo "sudo pacman -S mesa"
                    ;;
                libX11.so*)
                    echo "sudo pacman -S libx11"
                    ;;
                *)
                    echo "sudo pacman -S $(echo "$missing_lib" | sed 's/^lib//;s/\.so.*$//')"
                    ;;
            esac
            ;;
        debian|ubuntu|raspbian)
            # Tentar mapear biblioteca para pacote do Debian/Ubuntu
            case "$missing_lib" in
                libglfw.so*)
                    echo "sudo apt-get install libglfw3"
                    ;;
                libSDL2.so*)
                    echo "sudo apt-get install libsdl2-2.0-0"
                    ;;
                libpng*.so*)
                    echo "sudo apt-get install libpng16-16"
                    ;;
                libavcodec.so*)
                    echo "sudo apt-get install libavcodec-dev libavcodec60"
                    ;;
                libavformat.so*)
                    echo "sudo apt-get install libavformat-dev libavformat60"
                    ;;
                libavutil.so*)
                    echo "sudo apt-get install libavutil-dev libavutil58"
                    ;;
                libswscale.so*)
                    echo "sudo apt-get install libswscale-dev libswscale7"
                    ;;
                libswresample.so*)
                    echo "sudo apt-get install libswresample-dev libswresample4"
                    ;;
                libv4l2.so*)
                    echo "sudo apt-get install libv4l-0"
                    ;;
                libpulse.so*)
                    echo "sudo apt-get install libpulse0"
                    ;;
                libssl.so*|libcrypto.so*)
                    echo "sudo apt-get install libssl3"
                    ;;
                libGL.so*)
                    echo "sudo apt-get install libgl1-mesa-glx"
                    ;;
                libX11.so*)
                    echo "sudo apt-get install libx11-6"
                    ;;
                *)
                    echo "sudo apt-get install $(echo "$missing_lib" | sed 's/^lib//;s/\.so.*$//')"
                    ;;
            esac
            ;;
        fedora|rhel|centos)
            case "$missing_lib" in
                libglfw.so*)
                    echo "sudo dnf install glfw"
                    ;;
                libSDL2.so*)
                    echo "sudo dnf install SDL2"
                    ;;
                libpng*.so*)
                    echo "sudo dnf install libpng"
                    ;;
                libavcodec.so*|libavformat.so*|libavutil.so*|libswscale.so*|libswresample.so*)
                    echo "sudo dnf install ffmpeg-libs"
                    ;;
                libv4l2.so*)
                    echo "sudo dnf install v4l-utils"
                    ;;
                libpulse.so*)
                    echo "sudo dnf install pulseaudio-libs"
                    ;;
                libssl.so*|libcrypto.so*)
                    echo "sudo dnf install openssl-libs"
                    ;;
                libGL.so*)
                    echo "sudo dnf install mesa-libGL"
                    ;;
                libX11.so*)
                    echo "sudo dnf install libX11"
                    ;;
                *)
                    echo "sudo dnf install $(echo "$missing_lib" | sed 's/^lib//;s/\.so.*$//')"
                    ;;
            esac
            ;;
        *)
            echo "# Instale a biblioteca manualmente para: $missing_lib"
            ;;
    esac
}

# Detectar distribuição
detect_distro

# Encontrar executável
EXECUTABLE=""

if [ -n "$1" ]; then
    # Caminho fornecido como argumento
    EXECUTABLE="$1"
elif [ -f "build-linux-x86_64/bin/retrocapture" ]; then
    EXECUTABLE="build-linux-x86_64/bin/retrocapture"
elif [ -f "build-linux-arm64v8/bin/retrocapture" ]; then
    EXECUTABLE="build-linux-arm64v8/bin/retrocapture"
elif [ -f "build-linux-arm32v7/bin/retrocapture" ]; then
    EXECUTABLE="build-linux-arm32v7/bin/retrocapture"
elif [ -f "./bin/retrocapture" ]; then
    EXECUTABLE="./bin/retrocapture"
elif command -v retrocapture &> /dev/null; then
    EXECUTABLE="$(command -v retrocapture)"
else
    echo -e "${RED}❌ Executável não encontrado!${NC}"
    echo ""
    echo "Uso: $0 [caminho-do-executável]"
    echo ""
    echo "Locais verificados:"
    echo "  • build-linux-x86_64/bin/retrocapture"
    echo "  • build-linux-arm64v8/bin/retrocapture"
    echo "  • build-linux-arm32v7/bin/retrocapture"
    echo "  • ./bin/retrocapture"
    echo "  • retrocapture (no PATH)"
    exit 1
fi

# Verificar se o arquivo existe e é executável
if [ ! -f "$EXECUTABLE" ]; then
    echo -e "${RED}❌ Arquivo não encontrado: $EXECUTABLE${NC}"
    exit 1
fi

if [ ! -x "$EXECUTABLE" ]; then
    echo -e "${YELLOW}⚠️  Arquivo não é executável: $EXECUTABLE${NC}"
    echo "   Tentando continuar mesmo assim..."
fi

echo -e "${BLUE}=== Verificação de Dependências do RetroCapture ===${NC}"
echo ""
echo "📁 Executável: $EXECUTABLE"
echo "🖥️  Sistema: $DISTRO_NAME"
echo ""

# Verificar se ldd está disponível
if ! command -v ldd &> /dev/null; then
    echo -e "${RED}❌ ldd não está disponível!${NC}"
    echo "   Instale: glibc (geralmente já vem instalado)"
    exit 1
fi

# Executar ldd e capturar output
echo "🔍 Analisando dependências..."
echo ""

LDD_OUTPUT=$(ldd "$EXECUTABLE" 2>&1)
MISSING_LIBS=()
FOUND_LIBS=()

# Processar output do ldd
while IFS= read -r line; do
    # Verificar se a linha contém "not found"
    if echo "$line" | grep -q "not found"; then
        # Extrair nome da biblioteca
        LIB_NAME=$(echo "$line" | awk '{print $1}' | tr -d '=>')
        MISSING_LIBS+=("$LIB_NAME")
    elif echo "$line" | grep -q "=>"; then
        # Biblioteca encontrada
        LIB_NAME=$(echo "$line" | awk '{print $1}')
        FOUND_LIBS+=("$LIB_NAME")
    fi
done <<< "$LDD_OUTPUT"

# Mostrar resultado
if [ ${#MISSING_LIBS[@]} -eq 0 ]; then
    echo -e "${GREEN}✅ Todas as dependências estão instaladas!${NC}"
    echo ""
    echo "📋 Bibliotecas encontradas: ${#FOUND_LIBS[@]}"
    echo ""
    echo "Para ver todas as dependências:"
    echo "   ldd $EXECUTABLE"
else
    echo -e "${RED}❌ Encontradas ${#MISSING_LIBS[@]} dependência(s) faltando:${NC}"
    echo ""
    
    for lib in "${MISSING_LIBS[@]}"; do
        echo -e "${RED}   ✗ $lib${NC}"
    done
    
    echo ""
    echo -e "${YELLOW}💡 Comandos sugeridos para instalar as dependências faltantes:${NC}"
    echo ""
    
    # Agrupar sugestões por biblioteca
    for lib in "${MISSING_LIBS[@]}"; do
        INSTALL_CMD=$(suggest_install_command "$lib" "$lib")
        echo -e "${BLUE}   $INSTALL_CMD${NC}"
    done
    
    echo ""
    echo -e "${YELLOW}📋 Ou use o script de instalação automática:${NC}"
    case "$DISTRO_ID" in
        arch|manjaro|endeavouros)
            echo "   ./tools/install-deps-manjaro.sh"
            ;;
        debian|ubuntu|raspbian)
            echo "   ./tools/install-deps-raspberry-pi.sh"
            ;;
        *)
            echo "   Verifique a documentação para sua distribuição"
            ;;
    esac
fi

echo ""
echo -e "${BLUE}=== Informações Adicionais ===${NC}"
echo ""
echo "Para ver todas as dependências (incluindo caminhos):"
echo "   ldd $EXECUTABLE"
echo ""
echo "Para ver apenas bibliotecas necessárias:"
echo "   readelf -d $EXECUTABLE | grep NEEDED"
echo ""
echo "Para verificar arquitetura do executável:"
echo "   file $EXECUTABLE"
