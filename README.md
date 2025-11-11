# RetroCapture

Software de captura de vídeo para Linux com suporte a shaders do RetroArch em tempo real.

## Descrição

RetroCapture permite capturar vídeo de placas de captura no Linux e aplicar shaders do RetroArch (GLSL/Clang) em tempo real, proporcionando efeitos visuais retro e tratamento avançado de imagem.

## Características

- ✅ Captura de vídeo via V4L2
- ✅ Aplicação de shaders RetroArch em tempo real
- ✅ Suporte a shaders GLSL
- ✅ Suporte a shader presets (.glslp)
- ✅ Baixa latência
- ✅ Interface simples

## Requisitos

- Linux (com suporte V4L2)
- OpenGL 3.3+
- Placa de captura compatível com V4L2
- CMake 3.10+

## Dependências

- OpenGL
- GLFW ou SDL2
- libv4l2

## Build

### Requisitos de Build

- CMake 3.10+
- Compilador C++17 (g++, clang++)
- OpenGL development libraries
- GLFW3
- libv4l2
- pkg-config

### Instalação de Dependências (Arch Linux / Manjaro)

```bash
sudo pacman -S cmake gcc glfw libv4l pkg-config
```

### Compilação

```bash
# Usando o script de build
./build.sh

# Ou manualmente
mkdir build
cd build
cmake ..
make -j$(nproc)
```

O executável estará em `build/bin/retrocapture`

## Uso

```bash
# Executar (requer placa de captura em /dev/video0)
./build/bin/retrocapture

# Se sua placa estiver em outro dispositivo, edite o código
# ou adicione suporte a argumentos de linha de comando
```

### Requisitos de Execução

- Placa de captura conectada e acessível via V4L2
- Dispositivo geralmente em `/dev/video0`, `/dev/video1`, etc.
- Permissões de acesso ao dispositivo (pode precisar estar no grupo `video`)

## Status

✅ **Fase 1 - MVP Implementado:**
- ✅ Captura V4L2 funcional
- ✅ Janela OpenGL (GLFW)
- ✅ Pipeline de vídeo: captura → conversão → textura → renderização
- ✅ Exibição em tempo real

🚧 **Próximas Fases:**
- ⏳ Suporte a shaders RetroArch
- ⏳ Shader presets (.glslp)
- ⏳ Interface de configuração
- ⏳ Hotkeys

## Licença

[Definir licença]

