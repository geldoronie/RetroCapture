# Build e Teste no macOS

Este documento descreve como compilar e testar o RetroCapture no macOS 13 x86.

## Pré-requisitos

### 1. Homebrew

Se você ainda não tem o Homebrew instalado:

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

### 2. Instalar Dependências

Execute o script de instalação de dependências:

```bash
./tools/install-deps-macos.sh
```

Este script instala:
- **CMake** - Sistema de build
- **GLFW** - Gerenciamento de janelas
- **FFmpeg** - Streaming e encoding (libavcodec, libavformat, libavutil, libswscale, libswresample)
- **libpng** - Carregamento de imagens
- **pkg-config** - Detecção de dependências

## Build

### Build Release (Recomendado)

```bash
./tools/build-macos.sh Release
```

### Build Debug

```bash
./tools/build-macos.sh Debug
```

O executável será gerado em: `build-macos-$(uname -m)/bin/retrocapture`

## Teste

### Teste Rápido (Modo Dummy)

```bash
./tools/test-macos.sh
```

Este script testa o RetroCapture em modo dummy (sem dispositivo físico).

### Teste com Dispositivo Real

```bash
# Listar dispositivos disponíveis
./build-macos-$(uname -m)/bin/retrocapture --source avfoundation

# Capturar de dispositivo específico
./build-macos-$(uname -m)/bin/retrocapture \
  --source avfoundation \
  --width 1920 \
  --height 1080 \
  --fps 60
```

## Permissões Necessárias

O RetroCapture precisa de permissões do macOS para:

1. **Câmera**: System Preferences > Security & Privacy > Camera
2. **Microfone**: System Preferences > Security & Privacy > Microphone

Na primeira execução, o macOS solicitará essas permissões automaticamente.

## Solução de Problemas

### Erro: "Dependências faltando"

Execute:
```bash
./tools/install-deps-macos.sh
```

### Erro: "FFmpeg não encontrado"

Verifique se o FFmpeg está instalado:
```bash
brew list ffmpeg
pkg-config --modversion libavcodec
```

Se não estiver instalado:
```bash
brew install ffmpeg
```

### Erro: "GLFW não encontrado"

Verifique se o GLFW está instalado:
```bash
brew list glfw
pkg-config --modversion glfw3
```

Se não estiver instalado:
```bash
brew install glfw
```

### Erro de Compilação: "Objective-C++"

Certifique-se de que os arquivos `.mm` estão sendo compilados corretamente. O CMakeLists.txt já está configurado para isso.

### Erro: "Permissão negada para câmera"

1. Vá em System Preferences > Security & Privacy > Camera
2. Marque a opção para o RetroCapture
3. Reinicie o aplicativo

## Estrutura de Build

```
build-macos-$(uname -m)/
├── bin/
│   └── retrocapture          # Executável principal
├── shaders/                  # Shaders copiados
├── web/                      # Web portal copiado
├── ssl/                      # Certificados SSL copiados
└── assets/                   # Assets copiados
```

## Próximos Passos

Após o build bem-sucedido:

1. Teste a captura de vídeo com um dispositivo real
2. Teste a captura de áudio
3. Teste a aplicação de shaders
4. Teste o streaming HTTP MPEG-TS
5. Teste a gravação local

## Referências

- [AVFoundation Programming Guide](https://developer.apple.com/library/archive/documentation/AudioVideo/Conceptual/AVFoundationPG/)
- [Core Audio Overview](https://developer.apple.com/library/archive/documentation/MusicAudio/Conceptual/CoreAudioOverview/)
- [GLFW macOS Guide](https://www.glfw.org/docs/latest/build_guide.html#build_link_cmake)
