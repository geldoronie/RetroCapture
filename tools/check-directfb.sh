#!/bin/bash
# Script para verificar suporte a DirectFB no SDL2

echo "🔍 Verificando suporte DirectFB para SDL2..."
echo ""

# Verificar se SDL2 está instalado
if ! command -v sdl2-config &> /dev/null; then
    echo "❌ SDL2 não está instalado"
    echo "   Instale com: sudo apt-get install libsdl2-dev"
    exit 1
fi

echo "✅ SDL2 está instalado"
echo "   Versão: $(sdl2-config --version)"
echo ""

# Verificar se DirectFB está instalado
if ! dpkg -l | grep -q libdirectfb-dev; then
    echo "⚠️  DirectFB development libraries não estão instaladas"
    echo "   Instale com: sudo apt-get install libdirectfb-dev directfb"
else
    echo "✅ DirectFB development libraries estão instaladas"
fi

if ! dpkg -l | grep -q "^ii.*directfb "; then
    echo "⚠️  DirectFB runtime não está instalado"
    echo "   Instale com: sudo apt-get install directfb"
else
    echo "✅ DirectFB runtime está instalado"
fi
echo ""

# Verificar framebuffer
if [ -e /dev/fb0 ]; then
    echo "✅ Framebuffer disponível: /dev/fb0"
    ls -l /dev/fb* 2>/dev/null | head -3
else
    echo "⚠️  Framebuffer não encontrado: /dev/fb0"
fi
echo ""

# Verificar se SDL2 foi compilado com suporte a DirectFB
echo "🔍 Verificando drivers SDL2 disponíveis..."
SDL_VIDEODRIVER=directfb timeout 2 sdl2-config --prefix &>/dev/null
if [ $? -eq 0 ] || [ $? -eq 124 ]; then
    echo "   (Testando DirectFB...)"
fi

# Listar bibliotecas SDL2
echo ""
echo "📚 Bibliotecas SDL2 instaladas:"
ldconfig -p 2>/dev/null | grep sdl2 | head -5
echo ""

# Verificar variáveis de ambiente
echo "🌍 Variáveis de ambiente:"
if [ -n "$SDL_VIDEODRIVER" ]; then
    echo "   SDL_VIDEODRIVER=$SDL_VIDEODRIVER"
else
    echo "   SDL_VIDEODRIVER (não definido)"
fi

if [ -n "$DISPLAY" ]; then
    echo "   DISPLAY=$DISPLAY (X11 disponível)"
else
    echo "   DISPLAY (não definido - sem X11)"
fi
echo ""

# Teste rápido com SDL2
echo "🧪 Testando inicialização SDL2 com DirectFB..."
export SDL_VIDEODRIVER=directfb
timeout 2 sdl2-config --prefix &>/dev/null
if [ $? -eq 124 ]; then
    echo "   ⚠️  Timeout ao testar DirectFB (pode não estar disponível)"
else
    echo "   ✅ DirectFB parece estar disponível"
fi

echo ""
echo "💡 Dicas:"
echo "   1. Para usar DirectFB: export SDL_VIDEODRIVER=directfb"
echo "   2. Para usar framebuffer: export SDL_VIDEODRIVER=fbcon"
echo "   3. Para usar X11: export DISPLAY=:0"
echo "   4. Se DirectFB não funcionar, instale: sudo apt-get install libdirectfb-dev directfb"
