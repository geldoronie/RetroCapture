#!/bin/bash

build/bin/retrocapture --device /dev/video0 --width 1280 --height 720 --fps 60 --brightness 1.5 --contrast 1.5 --window-width 800 --window-height 600 --v4l2-brightness 70 --v4l2-contrast 50 --preset ./shaders/shaders_glsl/crt/crt-mattias.glslp