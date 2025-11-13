#!/bin/bash

# Script para gerar AppImage do RetroCapture

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== RetroCapture AppImage Builder ==="
echo ""

# Verificar se estamos no diretório correto
if [ ! -f "CMakeLists.txt" ]; then
    echo "Erro: Execute este script a partir do diretório raiz do projeto"
    exit 1
fi

# Versão da aplicação (obtida do CMakeLists.txt)
VERSION=$(grep -E "^project\(RetroCapture VERSION" CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+).*/\1/' || echo "0.1.0")
APP_NAME="RetroCapture"
APP_ID="com.retrocapture.app"
APP_DIR="AppDir"
APPIMAGE_NAME="${APP_NAME}-${VERSION}-x86_64.AppImage"

# Limpar builds anteriores
echo "Limpando builds anteriores..."
rm -rf "$APP_DIR"
rm -f "$APPIMAGE_NAME"

# Compilar a aplicação
echo ""
echo "=== Compilando aplicação ==="
./build.sh

# Criar estrutura AppDir
echo ""
echo "=== Criando estrutura AppDir ==="
mkdir -p "$APP_DIR/usr/bin"
mkdir -p "$APP_DIR/usr/lib"
mkdir -p "$APP_DIR/usr/share/applications"
mkdir -p "$APP_DIR/usr/share/icons/hicolor/256x256/apps"
mkdir -p "$APP_DIR/usr/share/retrocapture/shaders"

# Copiar executável
echo "Copiando executável..."
cp build/bin/retrocapture "$APP_DIR/usr/bin/"

# Copiar shaders
echo "Copiando shaders..."
if [ -d "shaders/shaders_glsl" ]; then
    cp -r shaders/shaders_glsl "$APP_DIR/usr/share/retrocapture/shaders/"
    echo "Shaders copiados: $(du -sh "$APP_DIR/usr/share/retrocapture/shaders" 2>/dev/null | cut -f1 || echo 'N/A')"
else
    echo "Aviso: Diretório shaders/shaders_glsl não encontrado"
fi

# Criar AppRun
echo "Criando AppRun..."
cat > "$APP_DIR/AppRun" << 'EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${0}")")"
export PATH="${HERE}/usr/bin:${PATH}"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${LD_LIBRARY_PATH}"
# Definir caminho base para shaders (dentro da AppImage)
export RETROCAPTURE_SHADER_PATH="${HERE}/usr/share/retrocapture/shaders/shaders_glsl"
exec "${HERE}/usr/bin/retrocapture" "$@"
EOF
chmod +x "$APP_DIR/AppRun"

# Criar arquivo .desktop
echo "Criando arquivo .desktop..."
cat > "$APP_DIR/usr/share/applications/retrocapture.desktop" << EOF
[Desktop Entry]
Type=Application
Name=RetroCapture
Comment=Video capture with RetroArch shader support
Exec=retrocapture
Icon=retrocapture
Categories=AudioVideo;Video;
Terminal=false
EOF

# Criar ícone simples (SVG placeholder)
echo "Criando ícone..."
cat > "$APP_DIR/usr/share/icons/hicolor/256x256/apps/retrocapture.svg" << 'EOF'
<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <rect width="256" height="256" fill="#2d2d2d"/>
  <text x="128" y="140" font-family="Arial, sans-serif" font-size="48" fill="#ffffff" text-anchor="middle" font-weight="bold">RC</text>
</svg>
EOF

# Criar ícone PNG simples (usando imagem básica)
# Se tiver um ícone real, substitua este comando
if command -v convert &> /dev/null; then
    convert -size 256x256 xc:#2d2d2d -pointsize 72 -fill white -gravity center -annotate +0+0 "RC" "$APP_DIR/usr/share/icons/hicolor/256x256/apps/retrocapture.png" 2>/dev/null || true
fi

# Verificar se linuxdeploy está disponível
LINUXDEPLOY=""
if command -v linuxdeploy &> /dev/null; then
    LINUXDEPLOY="linuxdeploy"
elif command -v linuxdeploy-x86_64.AppImage &> /dev/null; then
    LINUXDEPLOY="linuxdeploy-x86_64.AppImage"
else
    echo ""
    echo "=== linuxdeploy não encontrado ==="
    echo "Baixando linuxdeploy..."
    
    LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
    LINUXDEPLOY_APPIMAGE="linuxdeploy-x86_64.AppImage"
    
    if [ ! -f "$LINUXDEPLOY_APPIMAGE" ]; then
        wget -q "$LINUXDEPLOY_URL" -O "$LINUXDEPLOY_APPIMAGE"
        chmod +x "$LINUXDEPLOY_APPIMAGE"
    fi
    
    LINUXDEPLOY="./$LINUXDEPLOY_APPIMAGE"
fi

# Verificar se appimagetool está disponível (fallback)
APPIMAGETOOL=""
if command -v appimagetool &> /dev/null; then
    APPIMAGETOOL="appimagetool"
elif [ -f "appimagetool-x86_64.AppImage" ]; then
    APPIMAGETOOL="./appimagetool-x86_64.AppImage"
fi

# Usar linuxdeploy para copiar dependências e gerar AppImage
if [ -n "$LINUXDEPLOY" ]; then
    echo ""
    echo "=== Copiando dependências com linuxdeploy ==="
    
    # Verificar se ícone existe
    ICON_ARG=""
    if [ -f "$APP_DIR/usr/share/icons/hicolor/256x256/apps/retrocapture.png" ]; then
        ICON_ARG="--icon-file $APP_DIR/usr/share/icons/hicolor/256x256/apps/retrocapture.png"
    fi
    
    # Executar linuxdeploy para copiar bibliotecas
    if [ -n "$ICON_ARG" ]; then
        $LINUXDEPLOY \
            --appdir "$APP_DIR" \
            --executable "$APP_DIR/usr/bin/retrocapture" \
            --desktop-file "$APP_DIR/usr/share/applications/retrocapture.desktop" \
            $ICON_ARG \
            --output appimage 2>&1 || {
            echo "Tentando sem ícone..."
            $LINUXDEPLOY \
                --appdir "$APP_DIR" \
                --executable "$APP_DIR/usr/bin/retrocapture" \
                --desktop-file "$APP_DIR/usr/share/applications/retrocapture.desktop" \
                --output appimage 2>&1 || true
        }
    else
        $LINUXDEPLOY \
            --appdir "$APP_DIR" \
            --executable "$APP_DIR/usr/bin/retrocapture" \
            --desktop-file "$APP_DIR/usr/share/applications/retrocapture.desktop" \
            --output appimage 2>&1 || true
    fi
    
    # Renomear AppImage gerado
    if [ -f "RetroCapture-x86_64.AppImage" ]; then
        mv "RetroCapture-x86_64.AppImage" "$APPIMAGE_NAME"
        echo ""
        echo "=== AppImage gerada com sucesso! ==="
        echo "Arquivo: $APPIMAGE_NAME"
        echo "Tamanho: $(du -h "$APPIMAGE_NAME" | cut -f1)"
        exit 0
    fi
fi

# Fallback: usar appimagetool manualmente
if [ -n "$APPIMAGETOOL" ]; then
    echo ""
    echo "=== Gerando AppImage com appimagetool ==="
    
    # Copiar bibliotecas manualmente (usando ldd)
    echo "Copiando bibliotecas dependentes..."
    mkdir -p "$APP_DIR/usr/lib"
    
    # Função para copiar bibliotecas
    copy_deps() {
        local binary="$1"
        ldd "$binary" | grep "=>" | awk '{print $3}' | while read lib; do
            if [ -f "$lib" ]; then
                cp "$lib" "$APP_DIR/usr/lib/" 2>/dev/null || true
            fi
        done
    }
    
    copy_deps "$APP_DIR/usr/bin/retrocapture"
    
    # Gerar AppImage
    $APPIMAGETOOL "$APP_DIR" "$APPIMAGE_NAME"
    
    echo ""
    echo "=== AppImage gerada com sucesso! ==="
    echo "Arquivo: $APPIMAGE_NAME"
    exit 0
fi

# Se nenhuma ferramenta estiver disponível, criar estrutura básica
echo ""
echo "=== Aviso: Ferramentas de AppImage não encontradas ==="
echo "Estrutura AppDir criada em: $APP_DIR"
echo ""
echo "Para gerar a AppImage, você precisa de uma das seguintes ferramentas:"
echo ""
echo "Opção 1 - linuxdeploy (recomendado):"
echo "  wget https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
echo "  chmod +x linuxdeploy-x86_64.AppImage"
echo "  ./linuxdeploy-x86_64.AppImage --appdir $APP_DIR --executable $APP_DIR/usr/bin/retrocapture --desktop-file $APP_DIR/usr/share/applications/retrocapture.desktop --output appimage"
echo ""
echo "Opção 2 - appimagetool:"
echo "  wget https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage"
echo "  chmod +x appimagetool-x86_64.AppImage"
echo "  ./appimagetool $APP_DIR $APPIMAGE_NAME"
echo ""
echo "Estrutura criada. Copie manualmente as bibliotecas necessárias para $APP_DIR/usr/lib/"
echo "Use 'ldd build/bin/retrocapture' para ver as dependências."

