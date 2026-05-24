# Estratégia de Port para macOS 13 x86

## Visão Geral

Este documento descreve a estratégia para portar o RetroCapture para macOS 13 (x86/Intel), incluindo decisões técnicas, implementações necessárias e plano de ação.

## Análise do Estado Atual

### ✅ O que já está pronto

- **CMakeLists.txt**: Já detecta macOS e define `PLATFORM_MACOS`
- **Arquitetura de abstração**: Interfaces `IVideoCapture` e `IAudioCapture` facilitam o port
- **OpenGL**: Suportado no macOS (limitado a 4.1, deprecated mas funcional)
- **GLFW**: Suporta macOS nativamente
- **FFmpeg**: Suporta macOS via Homebrew
- **Dependências cross-platform**: ImGui, nlohmann/json funcionam no macOS

### ❌ O que precisa ser implementado

1. **VideoCaptureAVFoundation**: Captura de vídeo via AVFoundation
2. **AudioCaptureCoreAudio**: Captura de áudio via Core Audio
3. **Atualizações no CMakeLists.txt**: Dependências e linking específicos do macOS
4. **Atualizações nas Factories**: Suporte a macOS
5. **Ajustes no main.cpp**: Argumentos CLI para macOS
6. **Ajustes no Application.cpp**: Inicialização específica

## Decisões Técnicas

### 1. Captura de Vídeo: AVFoundation

**Por quê?**
- API nativa e oficial da Apple
- Suporta dispositivos UVC (USB Video Class)
- Boa performance e integração com o sistema
- Suporte a múltiplos formatos (YUYV, MJPEG, etc.)

**Implementação:**
- Usar `AVCaptureSession`, `AVCaptureDevice`, `AVCaptureVideoDataOutput`
- Converter `CVPixelBuffer` para formato compatível (YUYV ou RGB)
- Suportar listagem de dispositivos
- Implementar controles básicos (se disponíveis via AVCaptureDevice)

**Desafios:**
- AVFoundation é Objective-C, precisa de bridge para C++
- Permissões de câmera (NSCameraUsageDescription no Info.plist)
- Conversão de formatos pode ter overhead

### 2. Captura de Áudio: Core Audio

**Por quê?**
- API nativa do macOS
- Suporta captura de sistema (via BlackHole ou Loopback)
- Boa performance e baixa latência
- Compatível com o padrão do projeto

**Implementação:**
- Usar `AudioUnit` ou `AVAudioEngine` para captura
- Para captura de sistema, recomendar BlackHole (dispositivo virtual)
- Suportar listagem de dispositivos de áudio

**Desafios:**
- Core Audio é C/Objective-C, precisa de bridge
- Captura de sistema requer dispositivo virtual (BlackHole)
- Permissões de microfone (NSMicrophoneUsageDescription)

### 3. Renderização: OpenGL 4.1 (Temporário)

**Por quê?**
- OpenGL 4.1 ainda funciona no macOS 13
- Evita reescrever todos os shaders imediatamente
- Permite port funcional rápido

**Futuro:**
- Considerar migração para Metal (melhor performance, suporte oficial)
- Ou usar MoltenVK (Vulkan sobre Metal) para manter compatibilidade

## Plano de Implementação

### Fase 1: Implementação Mínima (Funcional)

**Objetivo**: Ter um build funcional que capture vídeo e áudio básico

1. ✅ Atualizar CMakeLists.txt
   - Adicionar frameworks: AVFoundation, CoreAudio, CoreVideo, VideoToolbox
   - Configurar linking correto
   - Excluir arquivos Linux/Windows do build macOS

2. ✅ Criar VideoCaptureAVFoundation
   - Implementar interface IVideoCapture
   - Captura básica de vídeo
   - Listagem de dispositivos
   - Modo dummy para testes

3. ✅ Criar AudioCaptureCoreAudio
   - Implementar interface IAudioCapture
   - Captura básica de áudio
   - Listagem de dispositivos
   - Suporte a BlackHole para captura de sistema

4. ✅ Atualizar Factories
   - VideoCaptureFactory: adicionar caso macOS
   - AudioCaptureFactory: adicionar caso macOS

5. ✅ Atualizar main.cpp
   - Adicionar source type "avfoundation" para macOS
   - Ajustar argumentos CLI

6. ✅ Atualizar Application.cpp
   - Inicialização específica do macOS
   - Device paths padrão

### Fase 2: Funcionalidades Completas

1. Controles de hardware (se disponíveis)
2. Permissões (câmera/microfone)
3. Testes completos
4. Documentação

### Fase 3: Otimizações (Futuro)

1. Migração para Metal (opcional)
2. Aceleração por hardware (VideoToolbox)
3. Melhorias de performance

## Estrutura de Arquivos

```
src/
├── capture/
│   ├── VideoCaptureAVFoundation.h
│   └── VideoCaptureAVFoundation.cpp
├── audio/
│   ├── AudioCaptureCoreAudio.h
│   └── AudioCaptureCoreAudio.cpp
```

## Dependências macOS

### Frameworks Necessários

- **AVFoundation**: Captura de vídeo
- **CoreAudio**: Captura de áudio
- **CoreVideo**: Processamento de vídeo
- **VideoToolbox**: Aceleração por hardware (futuro)
- **Foundation**: Base do Objective-C

### Bibliotecas Externas

- **FFmpeg**: Via Homebrew (`brew install ffmpeg`)
- **GLFW**: Via Homebrew (`brew install glfw`)
- **OpenGL**: Incluído no macOS
- **libpng**: Via Homebrew (`brew install libpng`)

## Permissões Necessárias

### Info.plist (para distribuição)

```xml
<key>NSCameraUsageDescription</key>
<string>RetroCapture precisa acessar a câmera para captura de vídeo</string>
<key>NSMicrophoneUsageDescription</key>
<string>RetroCapture precisa acessar o microfone para captura de áudio</string>
```

## Build no macOS

### Pré-requisitos

```bash
# Instalar Homebrew (se não tiver)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Instalar dependências
brew install cmake glfw ffmpeg libpng pkg-config
```

### Build

```bash
mkdir build && cd build
cmake ..
make -j$(sysctl -n hw.ncpu)
```

## Testes

### Testes Básicos

1. **Captura de vídeo**: Verificar se consegue listar e capturar de webcam
2. **Captura de áudio**: Verificar se consegue capturar áudio
3. **Shaders**: Testar aplicação de shaders básicos
4. **Streaming**: Testar streaming HTTP MPEG-TS
5. **Gravação**: Testar gravação local

## Limitações Conhecidas

1. **OpenGL deprecated**: Funciona mas pode ser removido no futuro
2. **Permissões**: Requer permissões de câmera/microfone
3. **Captura de sistema**: Requer BlackHole ou similar para áudio
4. **Controles de hardware**: Podem não estar disponíveis em todos os dispositivos

## Próximos Passos

1. Implementar VideoCaptureAVFoundation
2. Implementar AudioCaptureCoreAudio
3. Atualizar CMakeLists.txt
4. Atualizar Factories
5. Testar build e funcionalidades básicas

## Referências

- [AVFoundation Programming Guide](https://developer.apple.com/library/archive/documentation/AudioVideo/Conceptual/AVFoundationPG/)
- [Core Audio Overview](https://developer.apple.com/library/archive/documentation/MusicAudio/Conceptual/CoreAudioOverview/)
- [GLFW macOS Guide](https://www.glfw.org/docs/latest/build_guide.html#build_link_cmake)
