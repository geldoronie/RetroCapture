#!/bin/bash
set -e

echo "🧪 RetroCapture - Teste Rápido no macOS"
echo "======================================="
echo ""

# Detectar arquitetura
ARCH=$(uname -m)
BUILD_DIR="build-macos-$ARCH"

# Verificar se o executável existe
EXECUTABLE="$BUILD_DIR/bin/retrocapture"

if [ ! -f "$EXECUTABLE" ]; then
    echo "❌ Executável não encontrado: $EXECUTABLE"
    echo ""
    echo "   Execute primeiro o build:"
    echo "   ./tools/build-macos.sh"
    exit 1
fi

echo "✅ Executável encontrado: $EXECUTABLE"
echo ""

# Verificar permissões de câmera (se aplicável)
echo "📋 Verificando permissões..."
echo "   ⚠️  Certifique-se de que o RetroCapture tem permissão para:"
echo "      - Acessar a câmera (System Preferences > Security & Privacy > Camera)"
echo "      - Acessar o microfone (System Preferences > Security & Privacy > Microphone)"
echo ""

# Listar dispositivos disponíveis
echo "📹 Dispositivos de vídeo disponíveis:"
echo "   (Execute o RetroCapture para ver a lista completa)"
echo ""

# Teste básico: modo dummy
echo "🧪 Teste 1: Modo Dummy (sem dispositivo físico)"
echo "   Executando: $EXECUTABLE --source none --width 640 --height 480"
echo ""

"$EXECUTABLE" --source none --width 640 --height 480 --fps 30 &
PID=$!

sleep 3

if ps -p $PID > /dev/null; then
    echo "   ✅ RetroCapture iniciou com sucesso (PID: $PID)"
    echo "   Pressione Ctrl+C para parar o teste"
    echo ""
    wait $PID
else
    echo "   ❌ RetroCapture falhou ao iniciar"
    exit 1
fi

echo ""
echo "✅ Teste concluído!"
echo ""
echo "📝 Para testar com dispositivo real:"
echo "   $EXECUTABLE --source avfoundation --width 1920 --height 1080"
echo ""
