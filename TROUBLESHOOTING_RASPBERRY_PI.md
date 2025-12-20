# Troubleshooting - Raspberry Pi

## Erro: "Failed to create GLFW window"

### Solução 1: Verificar Display

```bash
# Verificar se DISPLAY está configurado
echo $DISPLAY

# Se estiver vazio, configure:
export DISPLAY=:0

# Ou se estiver via SSH:
export DISPLAY=:10.0  # ajuste conforme necessário
```

### Solução 2: Instalar Dependências Gráficas

```bash
# Execute o script de instalação atualizado
./install-deps-raspberry-pi.sh

# Ou instale manualmente:
sudo apt-get install -y \
    libglfw3-dev \
    libx11-dev \
    libxrandr-dev \
    libxinerama-dev \
    libxcursor-dev \
    libxi-dev \
    libgl1-mesa-dev \
    xorg-dev
```

### Solução 3: Usar Xvfb (Headless)

Se você não tem display físico ou está rodando via SSH:

```bash
# Instalar Xvfb
sudo apt-get install -y xvfb

# Rodar com display virtual
xvfb-run -a -s "-screen 0 1920x1080x24" ./retrocapture
```

### Solução 4: Verificar OpenGL

```bash
# Verificar se OpenGL está disponível
glxinfo 2>/dev/null | grep "OpenGL version" || echo "OpenGL não disponível"

# Se não estiver disponível, instale:
sudo apt-get install -y mesa-utils
```

### Solução 5: Verificar Permissões

```bash
# Se estiver usando Wayland, pode precisar:
export WAYLAND_DISPLAY=wayland-0

# Ou forçar X11:
export DISPLAY=:0
```

## Verificação Rápida

Execute estes comandos para diagnosticar:

```bash
# 1. Verificar display
echo "DISPLAY: $DISPLAY"

# 2. Verificar OpenGL
which glxinfo && glxinfo | grep "OpenGL version" || echo "glxinfo não instalado"

# 3. Verificar GLFW
ldconfig -p | grep glfw

# 4. Verificar X11
echo $XDG_SESSION_TYPE
ps aux | grep -E "Xorg|X11|wayland"
```

## Nota sobre OpenGL ES

A Raspberry Pi 3 usa OpenGL ES 2.0 via VideoCore IV. O código atual requer OpenGL 3.3 Core.

Se você tiver problemas persistentes, pode ser necessário:

- Usar Mesa com suporte a OpenGL desktop
- Ou modificar o código para suportar OpenGL ES como fallback
