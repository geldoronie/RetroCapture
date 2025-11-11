# Integração com Shaders RetroArch

## Visão Geral

O RetroCapture precisa ser compatível com o formato de shaders do RetroArch para aproveitar a vasta biblioteca de shaders existente. Este documento detalha como essa integração será implementada.

## Formato de Shaders RetroArch

### Shader Presets (.glslp, .slangp)

Os presets são arquivos de texto que definem múltiplos passes de shader. Exemplo:

```glslp
#reference "shaders/crt-lottes.glsl"
shader0 = "shaders/crt-lottes.glsl"
scale_type_x0 = "source"
scale_type_y0 = "source"
scale0 = "1.000000"
```

### Estrutura de um Shader Preset

1. **Referências**: `#reference` aponta para shaders base
2. **Passes**: Cada pass define um shader e seus parâmetros
3. **Scaling**: Como cada pass escala a imagem
4. **Texturas**: Texturas de referência (LUTs, etc.)

### Shader Simples (.glsl)

Um shader GLSL do RetroArch geralmente contém:

```glsl
#version 330

uniform sampler2D Texture;
uniform vec2 TextureSize;
uniform vec2 InputSize;
uniform vec2 OutputSize;

in vec2 TexCoord;
out vec4 FragColor;

void main() {
    // Código do shader
    FragColor = texture(Texture, TexCoord);
}
```

## Uniforms Padrão do RetroArch

RetroCapture precisa fornecer os seguintes uniforms:

| Uniform | Tipo | Descrição |
|---------|------|-----------|
| `IN.video_size` | vec2 | Tamanho do vídeo original |
| `IN.texture_size` | vec2 | Tamanho da textura atual |
| `IN.output_size` | vec2 | Tamanho de saída final |
| `IN.frame_count` | float | Contador de frames |
| `IN.frame_direction` | float | Direção do frame (1 ou -1) |
| `TIME` | float | Tempo decorrido em segundos |
| `FRAMEINDEX` | float | Índice do frame atual |

## Mapeamento de Uniforms

Como RetroArch usa uma estrutura `IN` para alguns uniforms, precisamos:

1. **Opção 1**: Traduzir os uniforms para nomes diretos
   - `IN.video_size` → `video_size`
   - Adicionar defines no shader

2. **Opção 2**: Manter compatibilidade total
   - Criar uma estrutura `IN` no shader
   - Passar uniforms como membros da estrutura

## Implementação Proposta

### Parser de Shader Preset

```cpp
class ShaderPreset {
    struct Pass {
        std::string shader_path;
        std::string scale_type_x;
        std::string scale_type_y;
        float scale;
        // ... outros parâmetros
    };
    
    std::vector<Pass> passes;
    std::vector<std::string> references;
    
    bool load(const std::string& path);
};
```

### Shader Engine

```cpp
class ShaderEngine {
    struct ShaderPass {
        GLuint program;
        GLuint framebuffer;
        GLuint texture;
        // ...
    };
    
    std::vector<ShaderPass> passes;
    
    bool loadPreset(const std::string& path);
    void render(GLuint input_texture);
};
```

## Passos de Renderização

1. **Captura**: Frame capturado → Textura OpenGL
2. **Pass 0**: Textura de entrada → Framebuffer 0
3. **Pass 1**: Framebuffer 0 → Framebuffer 1
4. **Pass N**: Framebuffer (N-1) → Framebuffer N
5. **Final**: Framebuffer final → Tela

## Texturas de Referência

Shaders do RetroArch frequentemente usam texturas de referência:

- **LUTs**: Look-up tables para correção de cor
- **Noise**: Texturas de ruído
- **Dither**: Padrões de dithering
- **Scanlines**: Padrões de scanline

Essas texturas geralmente estão em:
- `shaders/shaders_slang/`
- `shaders/shaders_glsl/`

## Desafios

1. **Compatibilidade de Versão GLSL**
   - RetroArch usa diferentes versões
   - Pode precisar traduzir/adaptar

2. **Samplers e Texturas**
   - RetroArch pode usar múltiplas texturas
   - Gerenciar binding points

3. **Precisão**
   - RetroArch usa `mediump` em alguns shaders
   - Garantir compatibilidade

4. **Vertex Shaders**
   - RetroArch tem vertex shaders padrão
   - Pode precisar fornecer um compatível

## Estratégia de Implementação

### Fase 1: Shader Simples
- Carregar shader .glsl único
- Aplicar com uniforms básicos
- Testar com shaders simples

### Fase 2: Shader Preset Básico
- Parser de .glslp simples
- Suporte a 2-3 passes
- Framebuffers intermediários

### Fase 3: Compatibilidade Completa
- Todos os uniforms do RetroArch
- Texturas de referência
- Múltiplos passes complexos
- Suporte a Slang (futuro)

## Testes

Shaders recomendados para teste:

1. **CRT Lottes**: Shader CRT popular
2. **CRT Royale**: Shader CRT avançado
3. **ScaleFX**: Upscaling
4. **Anti-aliasing**: Shaders de AA

## Referências

- [RetroArch Shader Format](https://github.com/libretro/slang-shaders)
- [GLSL Specification](https://www.khronos.org/opengl/wiki/OpenGL_Shading_Language)
- [RetroArch Documentation](https://docs.libretro.com/)

