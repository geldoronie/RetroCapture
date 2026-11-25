#!/bin/bash
# Script executado dentro do container Docker para gerar AppImage

set -e

BUILD_TYPE="${BUILD_TYPE:-Release}"
VERSION="${VERSION:-0.5.0-alpha}"
APP_NAME="RetroCapture"

# Usar diretório temporário para gerar AppImage (já que /work é read-only)
TEMP_DIR="/tmp/retrocapture-appimage"
APP_DIR="$TEMP_DIR/AppDir"
APPIMAGE_NAME="${APP_NAME}-${VERSION}-x86_64.AppImage"

# Diretório de saída (se especificado, copia o AppImage para lá)
OUTPUT_DIR="${OUTPUT_DIR:-/work}"

# Criar diretório temporário
mkdir -p "$TEMP_DIR"
cd "$TEMP_DIR"

echo "=== Gerando AppImage dentro do container ==="
echo "📦 Build type: $BUILD_TYPE"
echo "📦 Version: $VERSION"
echo "📦 Output dir: $OUTPUT_DIR"
echo "📦 Temp dir: $TEMP_DIR"
echo ""

# Verificar se estamos no diretório correto (verificar em /work)
if [ ! -f "/work/CMakeLists.txt" ]; then
    echo "❌ Erro: CMakeLists.txt não encontrado em /work!"
    echo "📋 Conteúdo de /work:"
    ls -la /work/ | head -20
    exit 1
fi

echo "✅ CMakeLists.txt encontrado em /work"

# Verificar e instalar dependências necessárias
echo "📋 Verificando dependências..."
if ! command -v file &> /dev/null; then
    echo "📥 Instalando 'file' (necessário para appimagetool)..."
    apt-get update -qq && apt-get install -y file > /dev/null 2>&1 || {
        echo "⚠️  Aviso: Não foi possível instalar 'file' automaticamente"
        echo "   O appimagetool pode falhar. Reconstrua a imagem Docker com 'file' instalado."
    }
fi
echo "✅ Dependências verificadas"

# Limpar builds anteriores
echo "🧹 Limpando builds anteriores..."
rm -rf "$APP_DIR"
rm -f "$APPIMAGE_NAME"
rm -f "${APP_NAME}"-*.AppImage

# Criar estrutura AppDir
echo ""
echo "=== Criando estrutura AppDir ==="
mkdir -p "$APP_DIR/usr/bin"
mkdir -p "$APP_DIR/usr/lib"
mkdir -p "$APP_DIR/usr/share/applications"
mkdir -p "$APP_DIR/usr/share/icons/hicolor/256x256/apps"
mkdir -p "$APP_DIR/usr/share/retrocapture/shaders"
mkdir -p "$APP_DIR/usr/share/retrocapture/assets"
mkdir -p "$APP_DIR/usr/share/retrocapture/web"
mkdir -p "$APP_DIR/usr/share/retrocapture/ssl"

# Copiar executável
echo "📋 Copiando executável..."
if [ ! -f "/work/build-linux-x86_64/bin/retrocapture" ]; then
    echo "❌ Erro: Executável não encontrado em /work/build-linux-x86_64/bin/retrocapture"
    exit 1
fi
cp /work/build-linux-x86_64/bin/retrocapture "$APP_DIR/usr/bin/"
chmod +x "$APP_DIR/usr/bin/retrocapture"
echo "✅ Executável copiado: $(ls -lh "$APP_DIR/usr/bin/retrocapture" | awk '{print $5}')"

# Copiar shaders
echo "📋 Copiando shaders..."
if [ -d "/work/shaders/shaders_glsl" ]; then
    cp -r /work/shaders/shaders_glsl "$APP_DIR/usr/share/retrocapture/shaders/"
    echo "✅ Shaders copiados"
else
    echo "⚠️  Aviso: Diretório /work/shaders/shaders_glsl não encontrado"
fi
# #251 — built-in upscaler passes (shaders/internal/upscalers); the app finds
# them next to RETROCAPTURE_SHADER_PATH (.../shaders/internal).
if [ -d "/work/shaders/internal" ]; then
    cp -r /work/shaders/internal "$APP_DIR/usr/share/retrocapture/shaders/"
    echo "✅ Internal shaders copied"
else
    echo "⚠️  Warning: /work/shaders/internal not found (built-in upscaler unavailable)"
fi

# Copiar assets
echo "📋 Copiando assets..."
if [ -d "/work/assets" ]; then
    cp -r /work/assets/* "$APP_DIR/usr/share/retrocapture/assets/" 2>/dev/null || true
    echo "✅ Assets copiados"
else
    echo "⚠️  Aviso: Diretório /work/assets não encontrado"
fi

# Copiar arquivos do web portal
echo "📋 Copiando arquivos do web portal..."
if [ -d "/work/build-linux-x86_64/bin/web" ]; then
    cp -r /work/build-linux-x86_64/bin/web/* "$APP_DIR/usr/share/retrocapture/web/" 2>/dev/null || true
    echo "✅ Web portal copiado"
elif [ -d "/work/src/web" ]; then
    cp -r /work/src/web/* "$APP_DIR/usr/share/retrocapture/web/" 2>/dev/null || true
    echo "✅ Web portal copiado"
else
    echo "⚠️  Aviso: Diretório web não encontrado"
fi

# Copiar arquivos SSL
echo "📋 Copiando arquivos SSL..."
if [ -d "/work/build-linux-x86_64/bin/ssl" ]; then
    cp -r /work/build-linux-x86_64/bin/ssl/* "$APP_DIR/usr/share/retrocapture/ssl/" 2>/dev/null || true
    echo "✅ SSL copiado"
elif [ -d "/work/ssl" ]; then
    cp -r /work/ssl/* "$APP_DIR/usr/share/retrocapture/ssl/" 2>/dev/null || true
    echo "✅ SSL copiado"
else
    echo "⚠️  Aviso: Diretório ssl não encontrado"
fi

# Criar AppRun
echo "📋 Criando AppRun..."
cat > "$APP_DIR/AppRun" << 'EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${0}")")"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${LD_LIBRARY_PATH}"
export RETROCAPTURE_SHADER_PATH="${HERE}/usr/share/retrocapture/shaders/shaders_glsl"
export RETROCAPTURE_ASSETS_PATH="${HERE}/usr/share/retrocapture/assets"
export RETROCAPTURE_WEB_PATH="${HERE}/usr/share/retrocapture/web"
cd "${HERE}/usr/bin"
exec "${HERE}/usr/bin/retrocapture" "$@"
EOF
chmod +x "$APP_DIR/AppRun"

# Criar arquivo .desktop
echo "📋 Criando arquivo .desktop..."
cat > "$APP_DIR/usr/share/applications/retrocapture.desktop" << EOF
[Desktop Entry]
Type=Application
Name=RetroCapture
Comment=Video capture with RetroArch shader support
Exec=retrocapture
Icon=retrocapture
Categories=AudioVideo;Video;
Terminal=false
StartupNotify=false
EOF
ln -sf usr/share/applications/retrocapture.desktop "$APP_DIR/${APP_NAME}.desktop" 2>/dev/null || true

# Copiar ícone
echo "📋 Copiando ícone..."
if [ -f "/work/assets/logo.png" ]; then
    cp "/work/assets/logo.png" "$APP_DIR/usr/share/icons/hicolor/256x256/apps/retrocapture.png"
    cp "/work/assets/logo.png" "$APP_DIR/retrocapture.png"
    echo "✅ Ícone copiado"
elif [ -f "/work/build-linux-x86_64/bin/assets/logo.png" ]; then
    cp "/work/build-linux-x86_64/bin/assets/logo.png" "$APP_DIR/usr/share/icons/hicolor/256x256/apps/retrocapture.png"
    cp "/work/build-linux-x86_64/bin/assets/logo.png" "$APP_DIR/retrocapture.png"
    echo "✅ Ícone copiado"
else
    echo "⚠️  Aviso: /work/assets/logo.png não encontrado"
fi

# Baixar linuxdeploy se necessário
echo ""
echo "=== Preparando ferramentas AppImage ==="
LINUXDEPLOY=""
LINUXDEPLOY_DIR="$TEMP_DIR/linuxdeploy-extracted"

if command -v linuxdeploy &> /dev/null; then
    LINUXDEPLOY="linuxdeploy"
    echo "✅ linuxdeploy encontrado no sistema"
elif [ -d "$LINUXDEPLOY_DIR" ] && [ -f "$LINUXDEPLOY_DIR/AppRun" ]; then
    LINUXDEPLOY="$LINUXDEPLOY_DIR/AppRun"
    echo "✅ linuxdeploy extraído encontrado em $LINUXDEPLOY_DIR"
elif [ -f "$TEMP_DIR/linuxdeploy-x86_64.AppImage" ]; then
    echo "📦 Extraindo linuxdeploy AppImage..."
    chmod +x "$TEMP_DIR/linuxdeploy-x86_64.AppImage"
    "$TEMP_DIR/linuxdeploy-x86_64.AppImage" --appimage-extract > /dev/null 2>&1
    mv squashfs-root "$LINUXDEPLOY_DIR"
    LINUXDEPLOY="$LINUXDEPLOY_DIR/AppRun"
    echo "✅ linuxdeploy extraído"
elif [ -f "/work/linuxdeploy-x86_64.AppImage" ]; then
    cp /work/linuxdeploy-x86_64.AppImage "$TEMP_DIR/"
    echo "📦 Extraindo linuxdeploy AppImage..."
    chmod +x "$TEMP_DIR/linuxdeploy-x86_64.AppImage"
    "$TEMP_DIR/linuxdeploy-x86_64.AppImage" --appimage-extract > /dev/null 2>&1
    mv squashfs-root "$LINUXDEPLOY_DIR"
    LINUXDEPLOY="$LINUXDEPLOY_DIR/AppRun"
    echo "✅ linuxdeploy extraído"
else
    echo "📥 Baixando linuxdeploy..."
    LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
    wget -q "$LINUXDEPLOY_URL" -O "$TEMP_DIR/linuxdeploy-x86_64.AppImage" || {
        echo "❌ Erro: Falha ao baixar linuxdeploy"
        exit 1
    }
    chmod +x "$TEMP_DIR/linuxdeploy-x86_64.AppImage"
    echo "📦 Extraindo linuxdeploy AppImage..."
    "$TEMP_DIR/linuxdeploy-x86_64.AppImage" --appimage-extract > /dev/null 2>&1
    mv squashfs-root "$LINUXDEPLOY_DIR"
    LINUXDEPLOY="$LINUXDEPLOY_DIR/AppRun"
    echo "✅ linuxdeploy baixado e extraído"
fi

# Baixar appimagetool se necessário
APPIMAGETOOL=""
APPIMAGETOOL_DIR="$TEMP_DIR/appimagetool-extracted"

if command -v appimagetool &> /dev/null; then
    APPIMAGETOOL="appimagetool"
    echo "✅ appimagetool encontrado no sistema"
elif [ -d "$APPIMAGETOOL_DIR" ] && [ -f "$APPIMAGETOOL_DIR/AppRun" ]; then
    APPIMAGETOOL="$APPIMAGETOOL_DIR/AppRun"
    echo "✅ appimagetool extraído encontrado em $APPIMAGETOOL_DIR"
elif [ -f "$TEMP_DIR/appimagetool-x86_64.AppImage" ]; then
    echo "📦 Extraindo appimagetool AppImage..."
    chmod +x "$TEMP_DIR/appimagetool-x86_64.AppImage"
    "$TEMP_DIR/appimagetool-x86_64.AppImage" --appimage-extract > /dev/null 2>&1
    mv squashfs-root "$APPIMAGETOOL_DIR"
    APPIMAGETOOL="$APPIMAGETOOL_DIR/AppRun"
    echo "✅ appimagetool extraído"
elif [ -f "/work/appimagetool-x86_64.AppImage" ]; then
    cp /work/appimagetool-x86_64.AppImage "$TEMP_DIR/"
    echo "📦 Extraindo appimagetool AppImage..."
    chmod +x "$TEMP_DIR/appimagetool-x86_64.AppImage"
    "$TEMP_DIR/appimagetool-x86_64.AppImage" --appimage-extract > /dev/null 2>&1
    mv squashfs-root "$APPIMAGETOOL_DIR"
    APPIMAGETOOL="$APPIMAGETOOL_DIR/AppRun"
    echo "✅ appimagetool extraído"
else
    echo "📥 Baixando appimagetool..."
    APPIMAGETOOL_URL="https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage"
    wget -q "$APPIMAGETOOL_URL" -O "$TEMP_DIR/appimagetool-x86_64.AppImage" || {
        echo "❌ Erro: Falha ao baixar appimagetool"
        exit 1
    }
    chmod +x "$TEMP_DIR/appimagetool-x86_64.AppImage"
    echo "📦 Extraindo appimagetool AppImage..."
    "$TEMP_DIR/appimagetool-x86_64.AppImage" --appimage-extract > /dev/null 2>&1
    mv squashfs-root "$APPIMAGETOOL_DIR"
    APPIMAGETOOL="$APPIMAGETOOL_DIR/AppRun"
    echo "✅ appimagetool baixado e extraído"
fi

# Usar linuxdeploy para copiar dependências
echo ""
echo "=== Copiando dependências com linuxdeploy ==="
ICON_ARG=""
if [ -f "$APP_DIR/usr/share/icons/hicolor/256x256/apps/retrocapture.png" ]; then
    ICON_ARG="--icon-file $APP_DIR/usr/share/icons/hicolor/256x256/apps/retrocapture.png"
fi

$LINUXDEPLOY \
    --appdir "$APP_DIR" \
    --executable "$APP_DIR/usr/bin/retrocapture" \
    --desktop-file "$APP_DIR/usr/share/applications/retrocapture.desktop" \
    $ICON_ARG \
    --output none 2>&1 || {
    echo "⚠️  Tentando sem ícone..."
    $LINUXDEPLOY \
        --appdir "$APP_DIR" \
        --executable "$APP_DIR/usr/bin/retrocapture" \
        --desktop-file "$APP_DIR/usr/share/applications/retrocapture.desktop" \
        --output none 2>&1 || true
}

# Verificar e restaurar AppRun se necessário
if [ -L "$APP_DIR/AppRun" ] || [ ! -f "$APP_DIR/AppRun" ] || ! head -1 "$APP_DIR/AppRun" | grep -q "#!/bin/bash"; then
    echo "🔧 Restaurando AppRun personalizado..."
    rm -f "$APP_DIR/AppRun"
    cat > "$APP_DIR/AppRun" << 'EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${0}")")"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${LD_LIBRARY_PATH}"
export RETROCAPTURE_SHADER_PATH="${HERE}/usr/share/retrocapture/shaders/shaders_glsl"
export RETROCAPTURE_ASSETS_PATH="${HERE}/usr/share/retrocapture/assets"
export RETROCAPTURE_WEB_PATH="${HERE}/usr/share/retrocapture/web"
cd "${HERE}/usr/bin"
exec "${HERE}/usr/bin/retrocapture" "$@"
EOF
    chmod +x "$APP_DIR/AppRun"
fi

# Gerar AppImage
echo ""
echo "=== Gerando AppImage ==="
echo "📋 AppDir: $APP_DIR"
echo "📋 AppImage: $APPIMAGE_NAME"
echo "📋 appimagetool: $APPIMAGETOOL"
echo "📋 Diretório de trabalho: $(pwd)"

# Verificar se AppDir existe e tem conteúdo
if [ ! -d "$APP_DIR" ]; then
    echo "❌ Erro: AppDir não existe: $APP_DIR"
    exit 1
fi

if [ ! -f "$APP_DIR/usr/bin/retrocapture" ]; then
    echo "❌ Erro: Executável não encontrado em $APP_DIR/usr/bin/retrocapture"
    exit 1
fi

export ARCH=x86_64
cd "$TEMP_DIR"
$APPIMAGETOOL "$APP_DIR" "$APPIMAGE_NAME" 2>&1

if [ -f "$TEMP_DIR/$APPIMAGE_NAME" ]; then
    # Mover para o nome correto se necessário
    if [ "$APPIMAGE_NAME" != "$(basename "$TEMP_DIR/$APPIMAGE_NAME")" ]; then
        mv "$TEMP_DIR/$APPIMAGE_NAME" "$TEMP_DIR/$APPIMAGE_NAME"
    fi
fi

if [ -f "$TEMP_DIR/$APPIMAGE_NAME" ]; then
    echo ""
    echo "✅ AppImage gerada com sucesso!"
    echo "📦 Arquivo: $APPIMAGE_NAME"
    echo "📏 Tamanho: $(du -h "$TEMP_DIR/$APPIMAGE_NAME" | cut -f1)"
    echo "📍 Localização temporária: $TEMP_DIR/$APPIMAGE_NAME"
    
    # Sempre copiar para o diretório de saída
    echo "📤 Copiando AppImage para $OUTPUT_DIR..."
    cp "$TEMP_DIR/$APPIMAGE_NAME" "$OUTPUT_DIR/" || {
        echo "❌ Erro: Falha ao copiar AppImage para $OUTPUT_DIR"
        echo "📋 Verificando permissões..."
        ls -la "$OUTPUT_DIR/" | head -5
        exit 1
    }
    
    if [ -f "$OUTPUT_DIR/$APPIMAGE_NAME" ]; then
        echo "✅ AppImage copiada para $OUTPUT_DIR/$APPIMAGE_NAME"
        echo "📏 Tamanho final: $(du -h "$OUTPUT_DIR/$APPIMAGE_NAME" | cut -f1)"
    else
        echo "❌ Erro: AppImage não foi copiada (arquivo não encontrado em $OUTPUT_DIR)"
        exit 1
    fi
    
    exit 0
else
    echo "❌ Erro: AppImage não foi gerada"
    echo "📋 Verificando conteúdo de $TEMP_DIR:"
    ls -la "$TEMP_DIR/" | head -10
    exit 1
fi
