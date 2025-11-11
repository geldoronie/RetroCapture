# Changelog

## [0.3.0] - 2025-11-11

### Adicionado
- ✅ Parâmetros de linha de comando para resolução (`--width`, `--height`)
- ✅ Parâmetro de linha de comando para framerate (`--fps`)
- ✅ Configuração dinâmica de captura V4L2
- ✅ Validação de parâmetros (limites de resolução e framerate)
- ✅ Detecção de capacidades do dispositivo V4L2
- ✅ Suporte a múltiplas resoluções (480p até 4K)

### Melhorado
- Mensagens de log mais informativas sobre configuração de captura
- Tratamento de erros quando resolução não é suportada
- Fallback gracioso quando framerate não pode ser configurado

## [0.2.0] - 2025-11-11

### Adicionado
- ✅ Suporte completo a shaders RetroArch (GLSL e Slang)
- ✅ Conversão automática de Slang (Vulkan) para GLSL (OpenGL 3.3)
- ✅ Suporte a múltiplos passes de shader
- ✅ Processamento de diretivas `#include` em shaders
- ✅ Carregamento de presets RetroArch (`.slangp`, `.glslp`)
- ✅ Uniforms do RetroArch: `SourceSize`, `OutputSize`, `OriginalSize`, `FrameCount`
- ✅ Parâmetros customizados com valores padrão
- ✅ Scaling types: `source`, `viewport`, `absolute`
- ✅ Argumentos de linha de comando: `--shader`, `--preset`, `--device`

### Corrigido
- ✅ Coordenadas de textura invertidas em shaders
- ✅ Framebuffer rendering para múltiplos passes
- ✅ Conversão de `push_constant` uniform blocks
- ✅ Remoção de `layout(location=X)` para compatibilidade GLSL 3.3
- ✅ Conversão de `uint` para `float` em uniforms
- ✅ Separação de vertex/fragment shaders em arquivos Slang únicos
- ✅ Resolução de paths relativos do RetroArch

### Técnico
- Conversão Slang→GLSL:
  - `#version 450` → `#version 330`
  - `layout(push_constant) uniform Push {...} params;` → uniforms individuais
  - `params.X` → `X`
  - `set =`, `binding =` removidos
  - `layout(location =)` removido
  - `uniform uint` → `uniform float`
  - `#pragma stage vertex/fragment` processado
  - `#include` recursivo com múltiplos caminhos de busca
  - `global.MVP` substituído por `Position` direto

## [0.1.0] - 2025-11-10

### Adicionado
- ✅ Captura de vídeo V4L2 em tempo real
- ✅ Renderização OpenGL 3.3+ com GLFW
- ✅ Conversão YUYV para RGB
- ✅ Sistema de logging
- ✅ Pipeline de baixa latência
- ✅ Memory mapping para buffers V4L2
- ✅ Non-blocking capture

### Inicial
- Estrutura base do projeto
- CMake build system
- Arquitetura modular (capture, renderer, shader engine)

