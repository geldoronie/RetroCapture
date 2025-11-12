# Recursos Faltantes - Análise Completa

## 📊 Estatísticas
- **Total de arquivos .glsl**: 576
- **Total de presets .glslp**: 482
- **Recursos identificados como faltantes**: 8 principais

## 🔴 CRÍTICO - Recursos Essenciais

### 1. **filter_linear#** - Filtragem de Texturas
**Status**: ✅ **IMPLEMENTADO**
**Implementação**: 
- Função `applyTextureSettings()` criada para aplicar configurações de textura
- Aplicado nas texturas de entrada dos passes
- Aplicado nas texturas de referência (LUTs, masks, etc)
- Suporta `GL_LINEAR` e `GL_NEAREST` conforme `filter_linear#`

### 2. **wrap_mode#** - Modo de Envolvimento de Texturas
**Status**: ✅ **IMPLEMENTADO**
**Implementação**:
- Função `wrapModeToGLEnum()` criada para converter strings para enums OpenGL
- Suporta: `clamp_to_edge`, `clamp_to_border`, `repeat`, `mirrored_repeat`
- Aplicado nas texturas de entrada dos passes
- Aplicado nas texturas de referência (LUTs, masks)

### 3. **Cópia Real de Frames para Histórico**
**Status**: ✅ **IMPLEMENTADO**
**Implementação**:
- Frames são copiados para texturas dedicadas usando framebuffer temporário
- Histórico mantém até 7 frames anteriores (MAX_FRAME_HISTORY)
- Usa renderização real em vez de apenas referências
- Suporta ring buffer para reutilização de texturas

## 🟡 IMPORTANTE - Recursos Comuns

### 4. **frame_count_mod#** - Módulo de FrameCount por Pass
**Status**: ✅ **IMPLEMENTADO**
**Implementação**:
- `frame_count_mod#` é armazenado em `ShaderPass.frameCountMod`
- Aplicado em `setupUniforms()` usando `fmod()`
- Suporta módulo por pass específico

### 5. **mipmap_input#** - Geração de Mipmaps
**Status**: ✅ **IMPLEMENTADO**
**Implementação**:
- Suportado em `applyTextureSettings()` com parâmetro `generateMipmap`
- Quando `mipmap_input# = true`, gera mipmaps usando `glGenerateMipmap()`
- Aplica filtros de mipmap apropriados (`GL_LINEAR_MIPMAP_LINEAR` ou `GL_NEAREST_MIPMAP_NEAREST`)

### 6. **srgb_framebuffer#** - Framebuffers sRGB
**Status**: ✅ **IMPLEMENTADO**
**Implementação**:
- Suportado em `createFramebuffer()` com parâmetro `srgbBuffer`
- Quando `srgb_framebuffer# = true`, usa `GL_SRGB8_ALPHA8` como formato interno
- Parsing já estava implementado em `ShaderPreset.cpp`

## 🟢 OPCIONAL - Recursos Avançados

### 7. **alias#** - Nomes de Passes
**Status**: Não implementado
**Problema**: Permite referenciar passes por nome em vez de índice.

**Exemplo**:
```
alias0 = "BloomPass"
alias1 = "ColorPass"
```

**O que fazer**:
- Armazenar alias no `ShaderPass`
- Permitir referenciar passes por alias em `PassPrev#Texture` ou similar

### 8. **COMPAT_* Macros** - Compatibilidade GLSL 1.20
**Status**: Não implementado
**Problema**: Alguns shaders antigos usam `COMPAT_TEXTURE`, `COMPAT_VARYING`, etc.

**O que fazer**:
- Adicionar defines no pré-processamento:
```cpp
#if __VERSION__ >= 130
#define COMPAT_VARYING out
#define COMPAT_ATTRIBUTE in
#define COMPAT_TEXTURE texture
#else
#define COMPAT_VARYING varying
#define COMPAT_ATTRIBUTE attribute
#define COMPAT_TEXTURE texture2D
#endif
```

## 📝 Prioridades de Implementação

### Alta Prioridade (Blocam shaders comuns):
1. ✅ **filter_linear#** - Aplicar nas texturas reais - **IMPLEMENTADO**
2. ✅ **wrap_mode#** - Necessário para muitos shaders - **IMPLEMENTADO**
3. ✅ **Cópia de frames** - Corrigir motion blur - **IMPLEMENTADO**

### Média Prioridade (Melhoram compatibilidade):
4. ✅ **frame_count_mod#** - Usado em vários shaders - **IMPLEMENTADO**
5. ✅ **mipmap_input#** - Usado em alguns shaders avançados - **IMPLEMENTADO**
6. ✅ **srgb_framebuffer#** - Usado em shaders de correção de cor - **IMPLEMENTADO**

### Baixa Prioridade (Recursos avançados):
7. **alias#** - Conveniência, não essencial
8. **COMPAT_* macros** - Compatibilidade com shaders muito antigos

## 🔍 Shaders Afetados

### Shaders que precisam de `filter_linear = false`:
- `motionblur-simple.glslp`
- `fast-bilateral*.glslp`
- `median_*.glslp`
- Muitos shaders de denoising

### Shaders que precisam de `wrap_mode = repeat`:
- Shaders de texturas procedurais
- Shaders de padrões repetitivos

### Shaders que precisam de `frame_count_mod`:
- Shaders com animações temporais
- Shaders com efeitos que alternam frames

## 🛠️ Plano de Ação

1. **Implementar filter_linear# corretamente** (1-2 horas)
2. **Implementar wrap_mode#** (1 hora)
3. **Corrigir cópia de frames** (2-3 horas)
4. **Implementar frame_count_mod#** (30 minutos)
5. **Implementar mipmap_input#** (1 hora)
6. **Implementar srgb_framebuffer#** (1 hora)

**Tempo total estimado**: 6-8 horas

