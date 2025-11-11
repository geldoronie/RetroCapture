#!/bin/bash
echo "=== Testando diferentes resoluções e framerates ==="
echo ""

echo "1. Teste com resolução 720p @ 30fps:"
echo "   ./build/bin/retrocapture --device /dev/video2 --width 1280 --height 720 --fps 30"
echo ""

echo "2. Teste com resolução 1080p @ 60fps (padrão):"
echo "   ./build/bin/retrocapture --device /dev/video2 --preset shaders/shaders_slang/crt/zfast-crt.slangp"
echo ""

echo "3. Teste com resolução 4K @ 30fps:"
echo "   ./build/bin/retrocapture --device /dev/video2 --width 3840 --height 2160 --fps 30"
echo ""

echo "4. Listar resoluções suportadas pelo dispositivo:"
echo "   v4l2-ctl --device /dev/video2 --list-formats-ext"
