# Usando SDL2 com DirectFB

## Compilação com SDL2

Para compilar o RetroCapture com suporte a SDL2 (e DirectFB/framebuffer), use a opção `BUILD_WITH_SDL2`:

```bash
cmake .. -DBUILD_WITH_SDL2=ON
cmake --build . -j$(nproc)
```

## Configuração do SDL2 para DirectFB

### Opção 1: Variável de Ambiente

```bash
# Usar DirectFB
export SDL_VIDEODRIVER=directfb
./retrocapture

# Usar framebuffer Linux
export SDL_VIDEODRIVER=fbcon
./retrocapture

# Usar X11 (fallback)
export SDL_VIDEODRIVER=x11
./retrocapture
```

### Opção 2: Auto-detecção

O `WindowManagerSDL` tenta automaticamente:

1. DirectFB (se disponível)
2. Framebuffer Linux (`fbcon`)
3. X11 (fallback)

## Requisitos

### DirectFB

```bash
sudo apt-get install -y libdirectfb-dev directfb
```

### Framebuffer

O framebuffer Linux geralmente já está disponível. Verifique:

```bash
ls -l /dev/fb*
```

## Uso na Raspberry Pi

Para usar DirectFB na Raspberry Pi:

```bash
# 1. Instalar DirectFB e SDL2
sudo apt-get install -y \
    libsdl2-dev \
    libdirectfb-dev \
    directfb

# 2. Compilar com SDL2
cmake .. -DBUILD_WITH_SDL2=ON
cmake --build . -j$(nproc)

# 3. Executar com DirectFB (recomendado para ARM)
export SDL_VIDEODRIVER=directfb
./retrocapture

# Ou usar framebuffer como alternativa
export SDL_VIDEODRIVER=fbcon
./retrocapture
```

### Melhorias para ARM

O RetroCapture agora detecta automaticamente sistemas ARM e:
- ✅ Prioriza OpenGL ES (mais comum em sistemas embarcados)
- ✅ Tenta DirectFB primeiro em sistemas ARM sem DISPLAY
- ✅ Fallback automático para framebuffer se DirectFB não estiver disponível
- ✅ Mensagens de erro específicas para sistemas ARM

## Verificação

Para verificar qual driver SDL2 está sendo usado:

```bash
# Verificar drivers disponíveis
sdl2-config --prefix
ldconfig -p | grep sdl2

# Testar SDL2
SDL_VIDEODRIVER=directfb ./retrocapture
```

## Notas

- SDL2 com DirectFB oferece melhor performance em sistemas embarcados
- Framebuffer é mais leve mas pode ter limitações de input
- X11 funciona como fallback se DirectFB/framebuffer não estiverem disponíveis
- OpenGL ES é suportado automaticamente quando disponível (Raspberry Pi)
