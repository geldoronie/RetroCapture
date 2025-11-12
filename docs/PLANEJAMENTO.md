# RetroCapture - Planejamento do Projeto

## Visão Geral

RetroCapture é um software de captura de vídeo para Linux que permite aplicar shaders do RetroArch (GLSL/Clang) em tempo real sobre o feed de vídeo de placas de captura, proporcionando efeitos visuais retro e tratamento de imagem avançado.

## Objetivos

1. Capturar vídeo de placas de captura no Linux (V4L2)
2. Aplicar shaders do RetroArch em tempo real
3. Suportar shaders GLSL e Clang
4. Interface simples e eficiente
5. Baixa latência para uso em tempo real

## Arquitetura do Sistema

### Componentes Principais

```
┌─────────────────────────────────────────────────────────┐
│                    RetroCapture                         │
├─────────────────────────────────────────────────────────┤
│                                                          │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────┐ │
│  │   Captura    │───▶│   Pipeline   │───▶│  Shader  │ │
│  │   (V4L2)     │    │   de Vídeo   │    │  Engine  │ │
│  ┌──────────────┘    ┌──────────────┘    ┌──────────┘ │
│                                                          │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────┐ │
│  │   Shader     │    │   Renderer   │    │  Output  │ │
│  │   Loader     │    │   (OpenGL)   │    │  (Display│ │
│  │              │    │              │    │   /File) │ │
│  ┌──────────────┘    ┌──────────────┘    ┌──────────┘ │
│                                                          │
└─────────────────────────────────────────────────────────┘
```

### Módulos

1. **VideoCapture (V4L2)**
   - Captura de frames da placa de captura
   - Suporte a múltiplos formatos (YUYV, MJPEG, H264, etc.)
   - Conversão de formatos quando necessário
   - Buffer circular para baixa latência

2. **Shader Engine**
   - Carregamento de shaders RetroArch (GLSL)
   - Parsing de arquivos .glsl e .slang
   - Gerenciamento de passes de shader
   - Uniforms e parâmetros configuráveis

3. **OpenGL Renderer**
   - Contexto OpenGL para renderização
   - Texturas para input/output
   - Framebuffers para passes intermediários
   - Pipeline de renderização otimizado

4. **Shader Loader**
   - Compatibilidade com formato RetroArch
   - Suporte a shader presets (.glslp, .slangp)
   - Carregamento de texturas de referência (LUTs, etc.)

5. **Output Manager**
   - Preview em janela (GLFW/SDL)
   - Gravação de vídeo (opcional)
   - Screenshots
   - Streaming (futuro)

6. **Configuration**
   - Arquivos de configuração
   - Hotkeys
   - Perfis de shader
   - Ajustes de captura

## Tecnologias e Dependências

### Core
- **C++17/20**: Linguagem principal
- **CMake**: Sistema de build
- **V4L2**: Captura de vídeo (via libv4l2)

### Graphics
- **OpenGL 3.3+**: Renderização e shaders
- **GLFW** ou **SDL2**: Gerenciamento de janela e contexto OpenGL
- **GLAD** ou **GLEW**: Loading de funções OpenGL

### Shader Support
- **GLSL**: Shaders do RetroArch
- **Slang**: Shaders Slang do RetroArch (via tradução ou suporte nativo)

### Utilities
- **spdlog**: Logging
- **nlohmann/json**: Configuração (opcional)
- **libavcodec/libavformat**: Para gravação de vídeo (opcional)

### Build Tools
- **pkg-config**: Detecção de dependências
- **Git**: Controle de versão

## Estrutura do Projeto

```
RetroCapture/
├── CMakeLists.txt
├── README.md
├── PLANEJAMENTO.md
├── LICENSE
├── src/
│   ├── main.cpp
│   ├── core/
│   │   ├── Application.cpp/h
│   │   └── Config.cpp/h
│   ├── capture/
│   │   ├── VideoCapture.cpp/h
│   │   ├── V4L2Capture.cpp/h
│   │   └── FrameBuffer.cpp/h
│   ├── shader/
│   │   ├── ShaderEngine.cpp/h
│   │   ├── ShaderLoader.cpp/h
│   │   ├── ShaderPass.cpp/h
│   │   └── RetroArchShader.cpp/h
│   ├── renderer/
│   │   ├── OpenGLRenderer.cpp/h
│   │   ├── Texture.cpp/h
│   │   ├── Framebuffer.cpp/h
│   │   └── QuadRenderer.cpp/h
│   ├── output/
│   │   ├── WindowManager.cpp/h
│   │   ├── VideoRecorder.cpp/h (opcional)
│   │   └── Screenshot.cpp/h
│   └── utils/
│       ├── Logger.cpp/h
│       └── Timer.cpp/h
├── shaders/
│   └── (shaders padrão ou exemplos)
├── config/
│   └── default.json
├── resources/
│   └── (texturas, ícones, etc.)
├── tests/
│   └── (testes unitários)
└── docs/
    └── (documentação adicional)
```

## Fluxo de Dados

```
1. V4L2 Capture
   └─▶ Frame Buffer (YUYV/MJPEG/etc)
       └─▶ Converter para RGB/RGBA
           └─▶ Upload para Textura OpenGL
               └─▶ Shader Pass 1
                   └─▶ Framebuffer Intermediário
                       └─▶ Shader Pass 2
                           └─▶ ...
                               └─▶ Shader Pass N (Final)
                                   └─▶ Render para Tela/Gravação
```

## Compatibilidade com Shaders RetroArch

### Formato de Shader RetroArch

RetroCapture precisa suportar:

1. **Shader Presets (.glslp, .slangp)**
   - Arquivos de texto que definem múltiplos passes
   - Parâmetros e uniforms
   - Texturas de referência

2. **Shaders Simples (.glsl, .slang)**
   - Shader único
   - Vertex + Fragment shader

3. **Uniforms Padrão**
   - `IN.video_size` - Tamanho do vídeo original
   - `IN.texture_size` - Tamanho da textura
   - `IN.output_size` - Tamanho de saída
   - `IN.frame_count` - Contador de frames
   - `IN.frame_direction` - Direção do frame
   - `TIME` - Tempo decorrido

4. **Texturas de Referência**
   - LUTs (Look-Up Tables)
   - Texturas de ruído
   - Texturas de scanlines

## Fases de Desenvolvimento

### Fase 1: Fundação (MVP)
- [ ] Setup do projeto CMake
- [ ] Captura básica V4L2
- [ ] Janela OpenGL simples
- [ ] Renderização básica do frame capturado
- [ ] Sistema de logging

### Fase 2: Shader Engine Básico
- [ ] Carregamento de shader GLSL simples
- [ ] Aplicação de shader em um frame
- [ ] Suporte a uniforms básicos
- [ ] Teste com shaders simples do RetroArch

### Fase 3: Shader Presets
- [ ] Parser de .glslp
- [ ] Suporte a múltiplos passes
- [ ] Framebuffers intermediários
- [ ] Texturas de referência

### Fase 4: Interface e Configuração
- [ ] Sistema de configuração
- [ ] Hotkeys (trocar shader, ajustes)
- [ ] Interface de seleção de shader
- [ ] Ajustes de parâmetros em tempo real

### Fase 5: Otimização e Features
- [ ] Otimização de performance
- [ ] Gravação de vídeo
- [ ] Screenshots
- [ ] Suporte a múltiplas placas de captura
- [ ] Suporte a Slang shaders

## Desafios Técnicos

1. **Latência**
   - Minimizar delay entre captura e exibição
   - Buffer otimizado
   - Threading adequado

2. **Compatibilidade de Shaders**
   - RetroArch usa convenções específicas
   - Pode precisar de adaptação/translação
   - Testar com shaders populares

3. **Performance**
   - Shaders complexos podem ser pesados
   - Otimização de passes
   - Uso eficiente de GPU

4. **Formatos de Vídeo**
   - Suporte a diferentes formatos de captura
   - Conversão eficiente
   - Hardware acceleration quando possível

## Considerações de Design

### Threading
- Thread principal: Renderização e UI
- Thread de captura: V4L2 polling/streaming
- Sincronização via mutex/atomic

### Memória
- Pool de buffers reutilizáveis
- Evitar alocações em loop de renderização
- Gerenciamento eficiente de texturas

### Extensibilidade
- Arquitetura modular
- Fácil adicionar novos formatos de output
- Plugin system (futuro)

## Próximos Passos Imediatos

1. Criar estrutura básica do projeto
2. Setup CMake com dependências
3. Implementar captura V4L2 básica
4. Criar janela OpenGL e renderizar frame simples
5. Testar com hardware real

## Referências

- RetroArch Shader Format: https://github.com/libretro/slang-shaders
- V4L2 API: https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/
- OpenGL Shading Language: https://www.khronos.org/opengl/wiki/OpenGL_Shading_Language

