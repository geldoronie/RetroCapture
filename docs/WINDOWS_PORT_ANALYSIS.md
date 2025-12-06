# Análise: Portabilidade do RetroCapture para Windows

## 📋 Resumo Executivo

Para compilar o RetroCapture no Windows, seria necessário substituir ou adaptar **3 componentes principais** específicos do Linux:

1. **V4L2 (Video4Linux2)** → **DirectShow / Media Foundation / Windows Camera API**
2. **PulseAudio** → **WASAPI (Windows Audio Session API)**
3. **pkg-config** → **CMake find_package / vcpkg / Conan**

## 🔴 Componentes Específicos do Linux que Precisam de Substituição

### 1. Captura de Vídeo (V4L2 → Windows)

**Arquivos Afetados:**

- `src/capture/VideoCapture.cpp/h`
- `src/utils/V4L2DeviceScanner.cpp/h`
- `src/v4l2/V4L2ControlMapper.cpp/h`
- `src/ui/UIConfigurationSource.cpp/h`

**Dependências Linux:**

- `libv4l2` (Video4Linux2)
- `linux/videodev2.h` header
- `/dev/video*` device paths
- V4L2 controls (brightness, contrast, saturation, etc.)

**Soluções Windows:**

#### Opção A: DirectShow (Recomendado para compatibilidade)

- **API**: DirectShow / COM
- **Biblioteca**: `strmiids.lib`, `ole32.lib`, `oleaut32.lib`
- **Vantagens**: Suporte amplo, funciona com webcams antigas
- **Desvantagens**: API complexa, COM-based

#### Opção B: Media Foundation (Windows 7+)

- **API**: Media Foundation (IMFMediaSource, etc.)
- **Biblioteca**: `mf.lib`, `mfplat.lib`, `mfuuid.lib`
- **Vantagens**: API moderna, melhor performance
- **Desvantagens**: Requer Windows 7+, não funciona com dispositivos antigos

#### Opção C: Windows Camera API (Windows 10+)

- **API**: Windows.Media.Capture (UWP) ou WinRT
- **Biblioteca**: Windows Runtime APIs
- **Vantagens**: API mais simples, suporte a múltiplas câmeras
- **Desvantagens**: Apenas Windows 10+, pode requerer UWP

**Recomendação**: Usar **Media Foundation** com fallback para **DirectShow** para máxima compatibilidade.

**Estrutura de Código Necessária:**

```cpp
// Abstração de captura de vídeo
class VideoCapture {
    #ifdef _WIN32
        // Implementação Windows (Media Foundation/DirectShow)
        IMFMediaSource* m_mediaSource;
        // ...
    #else
        // Implementação Linux (V4L2)
        int m_fd;
        struct v4l2_format m_format;
        // ...
    #endif
};
```

### 2. Captura de Áudio (PulseAudio → WASAPI)

**Arquivos Afetados:**

- `src/audio/AudioCapture.cpp/h`

**Dependências Linux:**

- `libpulse` (PulseAudio)
- `pulse/pulseaudio.h` headers
- `pa_mainloop`, `pa_context`, `pa_stream`

**Solução Windows: WASAPI (Windows Audio Session API)**

**API Windows:**

- **Biblioteca**: `avrt.lib`, `ole32.lib`
- **Headers**: `<mmdeviceapi.h>`, `<audioclient.h>`, `<endpointvolume.h>`
- **Método**: `IMMDeviceEnumerator`, `IAudioClient`, `IAudioCaptureClient`

**Estrutura de Código Necessária:**

```cpp
// Abstração de captura de áudio
class AudioCapture {
    #ifdef _WIN32
        // WASAPI
        IMMDeviceEnumerator* m_deviceEnumerator;
        IAudioClient* m_audioClient;
        IAudioCaptureClient* m_captureClient;
        // ...
    #else
        // PulseAudio
        pa_mainloop* m_mainloop;
        pa_context* m_context;
        pa_stream* m_stream;
        // ...
    #endif
};
```

### 3. Sistema de Build (pkg-config → CMake/vcpkg)

**Arquivos Afetados:**

- `CMakeLists.txt`

**Dependências Linux:**

- `pkg-config` para encontrar bibliotecas
- `pkg_check_modules()` no CMake

**Soluções Windows:**

#### Opção A: vcpkg (Recomendado)

- Gerenciador de pacotes C++ da Microsoft
- Suporta FFmpeg, OpenSSL, GLFW, libpng, etc.
- Integração fácil com CMake

#### Opção B: Conan

- Gerenciador de pacotes multiplataforma
- Suporta todas as dependências necessárias

#### Opção C: CMake find_package nativo

- Usar `find_package()` do CMake diretamente
- Requer instalação manual das bibliotecas

**Recomendação**: Usar **vcpkg** para gerenciar dependências no Windows.

## ✅ Componentes Multiplataforma (Não Precisam Mudança)

### 1. OpenGL e GLFW

- ✅ **GLFW**: Suporta Windows nativamente
- ✅ **OpenGL**: Suporta Windows via drivers de vídeo
- ✅ **GLAD**: Header-only, funciona em qualquer plataforma

### 2. FFmpeg

- ✅ **FFmpeg**: Compila nativamente no Windows
- ✅ Disponível via vcpkg: `vcpkg install ffmpeg`
- ✅ Mesmas APIs (libavcodec, libavformat, etc.)

### 3. OpenSSL

- ✅ **OpenSSL**: Compila nativamente no Windows
- ✅ Disponível via vcpkg: `vcpkg install openssl`
- ✅ Mesmas APIs

### 4. ImGui

- ✅ **ImGui**: Header-only, multiplataforma
- ✅ Backend GLFW+OpenGL funciona no Windows

### 5. nlohmann/json

- ✅ **nlohmann/json**: Header-only, multiplataforma

### 6. Streaming (HTTP Server)

- ✅ **HTTPServer.cpp**: Implementação própria, multiplataforma
- ✅ Usa sockets padrão (POSIX no Linux, Winsock2 no Windows)

### 7. Shaders

- ✅ **ShaderEngine**: Multiplataforma (OpenGL)
- ✅ **ShaderPreset**: Lógica de arquivo, multiplataforma

### 8. Processamento de Frame

- ✅ **FrameProcessor**: Lógica de processamento, multiplataforma

## 🔧 Mudanças Necessárias no Código

### 1. Headers Condicionais

```cpp
// VideoCapture.h
#ifdef _WIN32
    #include <windows.h>
    #include <mfapi.h>
    #include <mfidl.h>
    #include <mfreadwrite.h>
    #include <mferror.h>
    #pragma comment(lib, "mf.lib")
    #pragma comment(lib, "mfplat.lib")
    #pragma comment(lib, "mfuuid.lib")
#else
    #include <linux/videodev2.h>
    #include <sys/ioctl.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/mman.h>
#endif
```

### 2. CMakeLists.txt - Detecção de Plataforma

```cmake
if(WIN32)
    # Windows-specific dependencies
    find_package(OpenGL REQUIRED)
    find_package(glfw3 REQUIRED)
    find_package(FFmpeg REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(PNG REQUIRED)

    # Não precisa de V4L2 ou PulseAudio no Windows
    target_compile_definitions(retrocapture PRIVATE PLATFORM_WINDOWS)
else()
    # Linux-specific dependencies
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(V4L2 REQUIRED libv4l2)
    pkg_check_modules(PULSE REQUIRED libpulse)
    # ...
    target_compile_definitions(retrocapture PRIVATE PLATFORM_LINUX)
endif()
```

### 3. Abstração de Captura de Vídeo

Criar uma interface abstrata:

```cpp
// IVideoCapture.h (interface)
class IVideoCapture {
public:
    virtual ~IVideoCapture() = default;
    virtual bool open(const std::string& device) = 0;
    virtual bool setFormat(uint32_t width, uint32_t height) = 0;
    virtual bool captureFrame(Frame& frame) = 0;
    virtual bool setControl(const std::string& control, int32_t value) = 0;
    virtual std::vector<DeviceInfo> listDevices() = 0;
};

// VideoCaptureV4L2.h (Linux)
#ifdef PLATFORM_LINUX
class VideoCapture : public IVideoCapture {
    // Implementação V4L2
};
#endif

// VideoCaptureMF.h (Windows)
#ifdef PLATFORM_WINDOWS
class VideoCapture : public IVideoCapture {
    // Implementação Media Foundation
};
#endif
```

### 4. Abstração de Captura de Áudio

```cpp
// IAudioCapture.h (interface)
class IAudioCapture {
public:
    virtual ~IAudioCapture() = default;
    virtual bool open() = 0;
    virtual bool getSamples(std::vector<float>& samples) = 0;
    virtual void close() = 0;
};

// AudioCapturePulse.h (Linux)
#ifdef PLATFORM_LINUX
class AudioCapture : public IAudioCapture {
    // Implementação PulseAudio
};
#endif

// AudioCaptureWASAPI.h (Windows)
#ifdef PLATFORM_WINDOWS
class AudioCapture : public IAudioCapture {
    // Implementação WASAPI
};
#endif
```

## 📦 Dependências Windows (via vcpkg)

```bash
# Instalar vcpkg
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat

# Instalar dependências
.\vcpkg install glfw3:x64-windows
.\vcpkg install ffmpeg:x64-windows
.\vcpkg install openssl:x64-windows
.\vcpkg install libpng:x64-windows

# Integrar com CMake
.\vcpkg integrate install
```

## 🎯 Estratégia de Implementação Recomendada

### Fase 1: Preparação (1-2 semanas)

1. Criar interfaces abstratas (`IVideoCapture`, `IAudioCapture`)
2. Refatorar código atual para usar interfaces
3. Configurar CMake para detectar plataforma
4. Configurar vcpkg para Windows

### Fase 2: Implementação Windows (2-4 semanas)

1. Implementar `VideoCaptureMF` (Media Foundation)
2. Implementar `AudioCaptureWASAPI`
3. Adaptar `V4L2DeviceScanner` → `WindowsDeviceScanner`
4. Adaptar `V4L2ControlMapper` → `WindowsControlMapper` (se possível)

### Fase 3: Testes e Ajustes (1-2 semanas)

1. Testar captura de vídeo com diferentes webcams
2. Testar captura de áudio
3. Ajustar UI para diferenças de plataforma
4. Testar streaming e web portal

### Fase 4: Build e Distribuição (1 semana)

1. Criar script de build para Windows
2. Criar instalador (NSIS ou WiX)
3. Documentação de instalação

## ⚠️ Desafios e Limitações

### 1. Controles de Hardware

- **Linux**: V4L2 expõe controles de hardware (brightness, contrast, etc.)
- **Windows**: Media Foundation pode não expor todos os controles
- **Solução**: Usar `IAMCameraControl` e `IAMVideoProcAmp` do DirectShow como fallback

### 2. Descoberta de Dispositivos

- **Linux**: `/dev/video*` paths simples
- **Windows**: GUIDs complexos, requer enumeração via COM
- **Solução**: Criar wrapper que converte GUIDs para nomes amigáveis

### 3. Permissões

- **Linux**: Pode precisar de permissões para `/dev/video*`
- **Windows**: Pode precisar de permissões de câmera/microfone (Windows 10+)
- **Solução**: Documentar requisitos de permissão

### 4. Paths e Filesystem

- **Linux**: Paths Unix (`/dev/video0`, `/home/user/.config/`)
- **Windows**: Paths Windows (`\\?\`, `C:\Users\...`)
- **Solução**: Usar `std::filesystem` (C++17) que abstrai isso

## 📊 Estimativa de Esforço

| Tarefa                         | Complexidade | Tempo Estimado   |
| ------------------------------ | ------------ | ---------------- |
| Abstração de interfaces        | Média        | 1 semana         |
| Implementação Media Foundation | Alta         | 2 semanas        |
| Implementação WASAPI           | Média        | 1 semana         |
| Adaptação de UI                | Baixa        | 3 dias           |
| Adaptação de build system      | Média        | 3 dias           |
| Testes e ajustes               | Média        | 1 semana         |
| **Total**                      | -            | **~6-8 semanas** |

## 🚀 Conclusão

A portabilidade para Windows é **viável**, mas requer:

1. **Refatoração significativa** para abstrair componentes específicos do Linux
2. **Implementação de novos backends** para Windows (Media Foundation, WASAPI)
3. **Ajustes no sistema de build** (vcpkg, CMake condicional)
4. **Testes extensivos** com diferentes hardware

**Recomendação**: Começar criando as interfaces abstratas e refatorando o código atual antes de implementar os backends Windows. Isso facilitará manutenção futura e possíveis ports para macOS.
