#!/bin/bash
echo "Testando shader zfast-crt..."
timeout 3 ./build/bin/retrocapture --device /dev/video2 --preset ./shaders/shaders_slang/crt/zfast-crt.slangp 2>&1 | tail -5
echo ""
echo "Para testar outros shaders da pasta 1080p:"
echo "./build/bin/retrocapture --device /dev/video2 --preset ./shaders/1080p/01-1080p-crt-guest-advanced-hd-aperture-grille-u-normal-rf.slangp"
