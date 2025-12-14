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
# Suporta versões com sufixos como -alpha, -beta, etc.
VERSION=$(grep -E "^project\(RetroCapture VERSION" CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+[^ ]*).*/\1/' || echo "0.3.0-alpha")
APP_NAME="RetroCapture"
APP_ID="com.retrocapture.app"
APP_DIR="AppDir"
APPIMAGE_NAME="${APP_NAME}-${VERSION}-x86_64.AppImage"

# Limpar builds anteriores e artefatos
echo "Limpando builds anteriores e artefatos..."
rm -rf "$APP_DIR"
rm -f "$APPIMAGE_NAME"
# Limpar AppImages geradas anteriormente (com diferentes nomes possíveis)
rm -f "${APP_NAME}"-*.AppImage
rm -f "RetroCapture"*.AppImage
# Limpar arquivos temporários do appimagetool se foram baixados
if [ -f "appimagetool-x86_64.AppImage" ] && [ ! -f ".keep-appimagetool" ]; then
    echo "Removendo appimagetool baixado..."
    rm -f appimagetool-x86_64.AppImage
fi
# Limpar arquivos temporários do linuxdeploy se foram baixados
if [ -f "linuxdeploy-x86_64.AppImage" ] && [ ! -f ".keep-linuxdeploy" ]; then
    echo "Removendo linuxdeploy baixado..."
    rm -f linuxdeploy-x86_64.AppImage
fi
# Limpar diretório de plugins se existir
if [ -d "linuxdeploy-plugins" ]; then
    rm -rf linuxdeploy-plugins
fi

# Compilar a aplicação usando Docker
echo ""
echo "=== Compilando aplicação usando Docker ==="
if ! command -v docker &> /dev/null; then
    echo "Erro: Docker não está instalado. É necessário para compilar a aplicação."
    exit 1
fi

DOCKER_COMPOSE="docker-compose"
if ! command -v docker-compose &> /dev/null && docker compose version &> /dev/null; then
    DOCKER_COMPOSE="docker compose"
fi

echo "Construindo imagem Docker (se necessário)..."
$DOCKER_COMPOSE build build-linux > /dev/null 2>&1 || $DOCKER_COMPOSE build build-linux

echo "Compilando RetroCapture no container Docker..."
$DOCKER_COMPOSE run --rm build-linux > build-linux.log 2>&1

if [ $? -ne 0 ]; then
    echo "Erro: Falha na compilação. Verifique build-linux.log para mais detalhes."
    exit 1
fi

echo "Compilação concluída!"

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
echo "Copiando executável..."
if [ ! -f "build-linux/bin/retrocapture" ]; then
    echo "Erro: Executável não encontrado em build-linux/bin/retrocapture"
    echo "A compilação via Docker pode ter falhado. Verifique build-linux.log"
    exit 1
fi
cp build-linux/bin/retrocapture "$APP_DIR/usr/bin/"
chmod +x "$APP_DIR/usr/bin/retrocapture"
echo "Executável copiado: $(ls -lh "$APP_DIR/usr/bin/retrocapture" | awk '{print $5}')"

# Copiar shaders
echo "Copiando shaders..."
if [ -d "shaders/shaders_glsl" ]; then
    cp -r shaders/shaders_glsl "$APP_DIR/usr/share/retrocapture/shaders/"
    echo "Shaders copiados: $(du -sh "$APP_DIR/usr/share/retrocapture/shaders" 2>/dev/null | cut -f1 || echo 'N/A')"
else
    echo "Aviso: Diretório shaders/shaders_glsl não encontrado"
fi

# Copiar assets (logo, etc.)
echo "Copiando assets..."
if [ -d "assets" ]; then
    cp -r assets/* "$APP_DIR/usr/share/retrocapture/assets/" 2>/dev/null || true
    echo "Assets copiados: $(du -sh "$APP_DIR/usr/share/retrocapture/assets" 2>/dev/null | cut -f1 || echo 'N/A')"
else
    echo "Aviso: Diretório assets não encontrado"
fi

# Copiar arquivos do web portal
echo "Copiando arquivos do web portal..."
if [ -d "build/web" ]; then
    cp -r build/web/* "$APP_DIR/usr/share/retrocapture/web/" 2>/dev/null || true
    echo "Web portal copiado: $(du -sh "$APP_DIR/usr/share/retrocapture/web" 2>/dev/null | cut -f1 || echo 'N/A')"
    elif [ -d "src/web" ]; then
    cp -r src/web/* "$APP_DIR/usr/share/retrocapture/web/" 2>/dev/null || true
    echo "Web portal copiado: $(du -sh "$APP_DIR/usr/share/retrocapture/web" 2>/dev/null | cut -f1 || echo 'N/A')"
else
    echo "Aviso: Diretório web não encontrado (build/web ou src/web)"
fi

# Copiar arquivos SSL
echo "Copiando arquivos SSL..."
if [ -d "build/ssl" ]; then
    cp -r build/ssl/* "$APP_DIR/usr/share/retrocapture/ssl/" 2>/dev/null || true
    echo "SSL copiado: $(du -sh "$APP_DIR/usr/share/retrocapture/ssl" 2>/dev/null | cut -f1 || echo 'N/A')"
    elif [ -d "ssl" ]; then
    cp -r ssl/* "$APP_DIR/usr/share/retrocapture/ssl/" 2>/dev/null || true
    echo "SSL copiado: $(du -sh "$APP_DIR/usr/share/retrocapture/ssl" 2>/dev/null | cut -f1 || echo 'N/A')"
else
    echo "Aviso: Diretório ssl não encontrado (build/ssl ou ssl)"
fi

# Criar AppRun
echo "Criando AppRun..."
cat > "$APP_DIR/AppRun" << 'EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${0}")")"
# Não adicionar usr/bin ao PATH para evitar conflitos
# export PATH="${HERE}/usr/bin:${PATH}"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${LD_LIBRARY_PATH}"
# Definir caminho base para shaders (dentro da AppImage)
export RETROCAPTURE_SHADER_PATH="${HERE}/usr/share/retrocapture/shaders/shaders_glsl"
# Definir caminho base para assets (dentro da AppImage)
export RETROCAPTURE_ASSETS_PATH="${HERE}/usr/share/retrocapture/assets"
# Definir caminho base para web portal (dentro da AppImage)
export RETROCAPTURE_WEB_PATH="${HERE}/usr/share/retrocapture/web"
# Mudar para diretório do executável para caminhos relativos funcionarem
cd "${HERE}/usr/bin"
# Executar diretamente sem usar PATH para evitar conflitos
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
StartupNotify=false
EOF
# O appimagetool precisa de um link simbólico do arquivo desktop na raiz do AppDir
ln -sf usr/share/applications/retrocapture.desktop "$APP_DIR/${APP_NAME}.desktop" 2>/dev/null || true

# Copiar ícone (logo.png)
echo "Copiando ícone..."
if [ -f "assets/logo.png" ]; then
    # Copiar para o diretório de ícones
    cp "assets/logo.png" "$APP_DIR/usr/share/icons/hicolor/256x256/apps/retrocapture.png"
    # Também copiar para a raiz do AppDir (appimagetool procura lá também)
    cp "assets/logo.png" "$APP_DIR/retrocapture.png"
    echo "Ícone copiado de assets/logo.png"
    elif [ -f "build/assets/logo.png" ]; then
    cp "build/assets/logo.png" "$APP_DIR/usr/share/icons/hicolor/256x256/apps/retrocapture.png"
    cp "build/assets/logo.png" "$APP_DIR/retrocapture.png"
    echo "Ícone copiado de build/assets/logo.png"
else
    echo "Aviso: assets/logo.png não encontrado, criando ícone placeholder..."
    # Criar ícone SVG placeholder
    cat > "$APP_DIR/usr/share/icons/hicolor/256x256/apps/retrocapture.svg" << 'EOF'
<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <rect width="256" height="256" fill="#2d2d2d"/>
  <text x="128" y="140" font-family="Arial, sans-serif" font-size="48" fill="#ffffff" text-anchor="middle" font-weight="bold">RC</text>
</svg>
EOF
    # Criar ícone PNG simples (usando imagem básica)
    if command -v convert &> /dev/null; then
        convert -size 256x256 xc:#2d2d2d -pointsize 72 -fill white -gravity center -annotate +0+0 "RC" "$APP_DIR/usr/share/icons/hicolor/256x256/apps/retrocapture.png" 2>/dev/null || true
        cp "$APP_DIR/usr/share/icons/hicolor/256x256/apps/retrocapture.png" "$APP_DIR/retrocapture.png" 2>/dev/null || true
    fi
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
    
    # Nota: Os plugins do linuxdeploy (openssl, ffmpeg) não são estritamente necessários
    # O linuxdeploy já copia automaticamente as dependências do executável via ldd
    # Os plugins são úteis apenas se você quiser garantir versões específicas ou
    # incluir binários adicionais (como ffmpeg/ffprobe executáveis)
    # Por enquanto, vamos confiar no deployment automático do linuxdeploy
    
    # Verificar se o executável existe
    if [ ! -f "$APP_DIR/usr/bin/retrocapture" ]; then
        echo "Erro: Executável não encontrado em $APP_DIR/usr/bin/retrocapture"
        exit 1
    fi
    
    # Verificar se o AppRun existe e é um script
    if [ ! -f "$APP_DIR/AppRun" ]; then
        echo "Erro: AppRun não encontrado em $APP_DIR/AppRun"
        exit 1
    fi
    
    # Usar linuxdeploy APENAS para copiar dependências, NÃO para gerar AppImage
    # Isso evita que ele sobrescreva nosso AppRun
    echo "Usando linuxdeploy para copiar dependências..."
    if [ -n "$ICON_ARG" ]; then
        $LINUXDEPLOY \
        --appdir "$APP_DIR" \
        --executable "$APP_DIR/usr/bin/retrocapture" \
        --desktop-file "$APP_DIR/usr/share/applications/retrocapture.desktop" \
        $ICON_ARG \
        --output none 2>&1 || {
            echo "Tentando sem ícone..."
            $LINUXDEPLOY \
            --appdir "$APP_DIR" \
            --executable "$APP_DIR/usr/bin/retrocapture" \
            --desktop-file "$APP_DIR/usr/share/applications/retrocapture.desktop" \
            --output none 2>&1 || true
        }
    else
        $LINUXDEPLOY \
        --appdir "$APP_DIR" \
        --executable "$APP_DIR/usr/bin/retrocapture" \
        --desktop-file "$APP_DIR/usr/share/applications/retrocapture.desktop" \
        --output none 2>&1 || true
    fi
    
    # Verificar e restaurar o AppRun se necessário
    if [ -L "$APP_DIR/AppRun" ] || [ ! -f "$APP_DIR/AppRun" ] || ! head -1 "$APP_DIR/AppRun" | grep -q "#!/bin/bash"; then
        echo "Restaurando AppRun personalizado..."
        rm -f "$APP_DIR/AppRun"
        cat > "$APP_DIR/AppRun" << 'EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "${0}")")"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${LD_LIBRARY_PATH}"
# Definir caminho base para shaders (dentro da AppImage)
export RETROCAPTURE_SHADER_PATH="${HERE}/usr/share/retrocapture/shaders/shaders_glsl"
# Definir caminho base para assets (dentro da AppImage)
export RETROCAPTURE_ASSETS_PATH="${HERE}/usr/share/retrocapture/assets"
# Definir caminho base para web portal (dentro da AppImage)
export RETROCAPTURE_WEB_PATH="${HERE}/usr/share/retrocapture/web"
# Mudar para diretório do executável para caminhos relativos funcionarem
cd "${HERE}/usr/bin"
# Executar diretamente sem usar PATH para evitar conflitos
exec "${HERE}/usr/bin/retrocapture" "$@"
EOF
        chmod +x "$APP_DIR/AppRun"
        echo "AppRun restaurado"
    fi
    
    # Gerar AppImage usando appimagetool (que preserva o AppRun)
    echo ""
    echo "=== Gerando AppImage com appimagetool ==="
    
    # Verificar se appimagetool está disponível
    APPIMAGETOOL_CMD=""
    if command -v appimagetool &> /dev/null; then
        APPIMAGETOOL_CMD="appimagetool"
        elif [ -f "appimagetool-x86_64.AppImage" ]; then
        APPIMAGETOOL_CMD="./appimagetool-x86_64.AppImage"
    else
        echo "Baixando appimagetool..."
        wget -q "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage" -O appimagetool-x86_64.AppImage
        chmod +x appimagetool-x86_64.AppImage
        APPIMAGETOOL_CMD="./appimagetool-x86_64.AppImage"
    fi
    
    if [ -n "$APPIMAGETOOL_CMD" ]; then
        # Verificar se o arquivo desktop existe
        if [ ! -f "$APP_DIR/usr/share/applications/retrocapture.desktop" ]; then
            echo "Erro: Arquivo desktop não encontrado em $APP_DIR/usr/share/applications/retrocapture.desktop"
            exit 1
        fi
        
        # O appimagetool precisa encontrar o arquivo desktop
        # Vamos garantir que ele existe e está acessível
        export ARCH=x86_64
        # O appimagetool procura por arquivos .desktop em usr/share/applications/
        # e usa o primeiro que encontrar ou o que corresponder ao nome do AppDir
        $APPIMAGETOOL_CMD "$APP_DIR" "$APPIMAGE_NAME" 2>&1
        if [ -f "$APPIMAGE_NAME" ]; then
            echo ""
            echo "=== AppImage gerada com sucesso! ==="
            echo "Arquivo: $APPIMAGE_NAME"
            echo "Tamanho: $(du -h "$APPIMAGE_NAME" | cut -f1)"
            
            # Limpar AppDir após sucesso (opcional, pode ser útil manter para debug)
            # Descomente a linha abaixo se quiser limpar automaticamente após sucesso
            # rm -rf "$APP_DIR"
            
            exit 0
        else
            echo "Erro: appimagetool não gerou a AppImage"
        fi
    else
        echo "Erro: appimagetool não encontrado e não foi possível baixá-lo"
        echo "Continuando com fallback..."
    fi
fi

# Limpeza final em caso de erro ou se chegou aqui
# (mantém AppDir para debug se houver erro)
if [ $? -ne 0 ]; then
    echo ""
    echo "Aviso: Build falhou. AppDir mantido para debug em: $APP_DIR"
    echo "Execute 'rm -rf $APP_DIR' para limpar manualmente"
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
    
    # Usar o executável do build-linux para copiar dependências
    if [ -f "build-linux/bin/retrocapture" ]; then
        copy_deps "build-linux/bin/retrocapture"
    else
        copy_deps "$APP_DIR/usr/bin/retrocapture"
    fi
    
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
echo "Use 'ldd build-linux/bin/retrocapture' para ver as dependências."

