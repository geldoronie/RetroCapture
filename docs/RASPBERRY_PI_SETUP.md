# Configuração para Raspberry Pi

## Problemas Comuns e Soluções

### Erro: "Failed to create GLFW window"

Este erro geralmente ocorre por uma das seguintes razões:

#### 1. Display não configurado (headless)

Se você está rodando via SSH sem X11 forwarding:

```bash
# Verificar se DISPLAY está configurado
echo $DISPLAY

# Se estiver vazio, você precisa:
# - Rodar com X11 forwarding: ssh -X user@raspberrypi
# - Ou configurar DISPLAY: export DISPLAY=:0
# - Ou usar xvfb para virtual display
```

**Solução: Usar Xvfb (Virtual Framebuffer)**

```bash
# Instalar Xvfb
sudo apt-get install -y xvfb

# Rodar com display virtual
xvfb-run -a ./retrocapture
```

#### 2. OpenGL não disponível

A Raspberry Pi usa OpenGL ES, não OpenGL desktop. O código atual requer OpenGL 3.3 Core.

**Verificar suporte OpenGL:**

```bash
# Verificar drivers Mesa/OpenGL
glxinfo | grep "OpenGL version"
# ou
/opt/vc/bin/vcgencmd version

# Verificar se Mesa está instalado
dpkg -l | grep mesa
```

**Instalar dependências gráficas:**

```bash
sudo apt-get install -y \
    libgl1-mesa-dev \
    libgles2-mesa-dev \
    libegl1-mesa-dev \
    xorg-dev
```

#### 3. Dependências faltando

```bash
# Instalar todas as dependências necessárias
sudo apt-get install -y \
    libglfw3-dev \
    libx11-dev \
    libxrandr-dev \
    libxinerama-dev \
    libxcursor-dev \
    libxi-dev \
    libgl1-mesa-dev \
    libgles2-mesa-dev
```

#### 4. Rodar com permissões corretas

```bash
# Se estiver usando Wayland, pode precisar:
export WAYLAND_DISPLAY=wayland-0

# Ou forçar X11:
export DISPLAY=:0
```

## Modo Headless (Sem Display)

Para rodar sem display físico (útil para streaming apenas):

```bash
# Instalar Xvfb
sudo apt-get install -y xvfb x11vnc

# Rodar com display virtual
xvfb-run -a -s "-screen 0 1920x1080x24" ./retrocapture --source v4l2
```

## Verificação Rápida

```bash
# 1. Verificar display
echo $DISPLAY

# 2. Verificar OpenGL
glxinfo 2>/dev/null | grep "OpenGL version" || echo "OpenGL não disponível"

# 3. Verificar GLFW
ldconfig -p | grep glfw

# 4. Testar criação de janela simples
# (pode criar um teste simples com GLFW)
```

## Suporte SDL2 com DirectFB (Recomendado para ARM)

Para melhor suporte em sistemas ARM embarcados como Raspberry Pi, use SDL2 com DirectFB:

### Compilação com SDL2

```bash
# Compilar com suporte SDL2/DirectFB
cmake .. -DBUILD_WITH_SDL2=ON
cmake --build . -j$(nproc)
```

### Instalar DirectFB

```bash
# Instalar DirectFB e dependências
sudo apt-get install -y \
    libsdl2-dev \
    libdirectfb-dev \
    directfb
```

### Usar DirectFB

```bash
# Executar com DirectFB (recomendado para sistemas embarcados ARM)
export SDL_VIDEODRIVER=directfb
./retrocapture

# Ou usar framebuffer Linux
export SDL_VIDEODRIVER=fbcon
./retrocapture
```

### Vantagens do SDL2 + DirectFB

- ✅ Suporte nativo a OpenGL ES (comum em ARM)
- ✅ Acesso direto ao framebuffer sem X11
- ✅ Melhor performance em sistemas embarcados
- ✅ Detecção automática de ARM e priorização de OpenGL ES
- ✅ Fallback automático para framebuffer se DirectFB não estiver disponível

## Nota sobre OpenGL ES

A Raspberry Pi 3 usa VideoCore IV que suporta OpenGL ES 2.0. Com SDL2, o RetroCapture detecta automaticamente sistemas ARM e tenta criar um contexto OpenGL ES primeiro, com fallback para OpenGL desktop se necessário.

**Recomendação:** Use `-DBUILD_WITH_SDL2=ON` ao compilar para Raspberry Pi para melhor compatibilidade.
