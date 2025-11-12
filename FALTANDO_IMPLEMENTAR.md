# Recursos Faltantes - Análise Completa

## 📊 Estatísticas
- **Total de arquivos .glsl**: 576
- **Total de presets .glslp**: 482
- **Recursos identificados como faltantes**: 8 principais

## 🔴 CRÍTICO - Recursos Essenciais

### 1. **filter_linear#** - Filtragem de Texturas
**Status**: Parcialmente implementado (apenas como uniform)
**Problema**: O `filter_linear#` está sendo passado como uniform, mas não está sendo aplicado nas texturas reais.

**O que fazer**:
- Quando `filter_linear# = false`, usar `GL_NEAREST` em vez de `GL_LINEAR`
- Aplicar nas texturas de entrada dos passes (inputTexture)
- Aplicar nas texturas de referência (LUTs, masks, etc)

**Código atual**:
```cpp
// Apenas passa como uniform, não aplica na textura
glUniform1f(loc, passInfo.filterLinear ? 1.0f : 0.0f);
```

**Código necessário**:
```cpp
// Aplicar filtro na textura de entrada
GLenum filter = passInfo.filterLinear ? GL_LINEAR : GL_NEAREST;
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
```

### 2. **wrap_mode#** - Modo de Envolvimento de Texturas
**Status**: Não implementado
**Problema**: Texturas sempre usam `GL_CLAMP_TO_EDGE`, mas alguns shaders precisam de `GL_REPEAT` ou `GL_MIRRORED_REPEAT`.

**Valores possíveis**:
- `clamp_to_edge` → `GL_CLAMP_TO_EDGE`
- `clamp_to_border` → `GL_CLAMP_TO_BORDER`
- `repeat` → `GL_REPEAT`
- `mirrored_repeat` → `GL_MIRRORED_REPEAT`

**Onde aplicar**:
- Texturas de entrada dos passes
- Texturas de referência (LUTs, masks)

### 3. **Cópia Real de Frames para Histórico**
**Status**: Implementado parcialmente (usa renderização, mas pode não estar funcionando)
**Problema**: O motion blur não funciona porque os frames não estão sendo copiados corretamente.

**O que fazer**:
- Garantir que a renderização para o framebuffer temporário está funcionando
- Verificar se o shader program usado para cópia está correto
- Adicionar logs para debug

## 🟡 IMPORTANTE - Recursos Comuns

### 4. **frame_count_mod#** - Módulo de FrameCount por Pass
**Status**: Não implementado
**Problema**: Alguns shaders precisam que `FrameCount` seja aplicado com um módulo específico por pass.

**Exemplo**:
```
frame_count_mod0 = 2  // FrameCount % 2 para pass 0
```

**O que fazer**:
- Armazenar `frame_count_mod#` no `ShaderPass`
- Aplicar módulo em `setupUniforms`:
```cpp
float frameCount = m_frameCount;
if (passInfo.frameCountMod > 0) {
    frameCount = fmod(m_frameCount, passInfo.frameCountMod);
}
glUniform1f(frameCountLoc, frameCount);
```

### 5. **mipmap_input#** - Geração de Mipmaps
**Status**: Não implementado
**Problema**: Alguns shaders precisam de mipmaps nas texturas de entrada.

**O que fazer**:
- Quando `mipmap_input# = true`, gerar mipmaps:
```cpp
if (passInfo.mipmapInput) {
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
}
```

### 6. **srgb_framebuffer#** - Framebuffers sRGB
**Status**: Não implementado
**Problema**: Alguns shaders precisam de framebuffers sRGB para correção de cor.

**O que fazer**:
- Quando `srgb_framebuffer# = true`, usar formato sRGB:
```cpp
GLenum internalFormat = passInfo.srgbFramebuffer ? GL_SRGB8_ALPHA8 : GL_RGBA;
```

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
1. ✅ **filter_linear#** - Aplicar nas texturas reais
2. ✅ **wrap_mode#** - Necessário para muitos shaders
3. ✅ **Cópia de frames** - Corrigir motion blur

### Média Prioridade (Melhoram compatibilidade):
4. **frame_count_mod#** - Usado em vários shaders
5. **mipmap_input#** - Usado em alguns shaders avançados
6. **srgb_framebuffer#** - Usado em shaders de correção de cor

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

