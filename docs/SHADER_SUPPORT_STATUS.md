# RetroArch Shader Support Status

## ✅ Funcionalidades Implementadas

### Conversão Slang → GLSL
- [x] `#version 450` → `#version 330`
- [x] `layout(push_constant)` → `uniform` individuais
- [x] Remoção de `set =`, `binding =`, `layout(location =)`
- [x] `params.X` → `X` (substituição global)
- [x] `uniform uint` → `uniform float`
- [x] `#pragma stage vertex/fragment` (separação de stages)
- [x] `#include` recursivo com resolução de paths
- [x] `global.MVP` → `Position` direto
- [x] **Extração de parâmetros do bloco UBO**
- [x] **Processamento de `#pragma parameter`**
- [x] **Suporte a `OriginalHistorySize#` (history buffers)**

### Uniforms RetroArch
- [x] `SourceSize`, `OutputSize`, `OriginalSize`
- [x] `FrameCount`
- [x] `Texture` / `Source` samplers
- [x] **`OriginalHistorySize0-7`** (dummy values)
- [x] Parâmetros customizados (`#pragma parameter`)
- [x] Parâmetros do bloco UBO

### Parâmetros Customizados com Valores Padrão
- [x] `BLURSCALEX`, `LOWLUMSCAN`, `HILUMSCAN`
- [x] `BRIGHTBOOST`, `MASK_DARK`, `MASK_FADE`
- [x] `RESSWITCH_*` (glitch effects)
- [x] `AS`, `asat` (afterglow)
- [x] `PR`, `PG`, `PB` (persistence)
- [x] `internal_res`, `auto_res`

## ✅ Shaders Testados com Sucesso

### Simples
- ✅ `test_passthrough.slangp` - Passthrough básico
- ✅ `crt/zfast-crt.slangp` - CRT rápido

### Complexos (12 passes)
- ✅ `1080p/05-1080p-crt-guest-advanced-hd-aperture-grille-u-normal-rgb.slangp`
  - Pass 0: `stock.slang` ✅
  - Pass 1: `crt-resswitch-glitch-koko.slang` ✅
  - Pass 2: `afterglow0.slang` ✅
  - Pass 3: `pre-shaders-afterglow-grade.slang` ✅
  - Pass 4: `linearize-hd.slang` ✅
  - Pass 5-11: Outros passes ✅

## 🚧 Limitações Conhecidas

### History Buffers (Frames Anteriores)
- **Status**: Declarados mas não implementados
- **Impacto**: Efeitos de afterglow/motion blur não funcionam corretamente
- **Workaround**: Usa dimensões do frame atual como dummy values
- **Necessário para**:
  - Afterglow effects
  - Motion blur
  - Response time emulation
  - Frame mixing

### Pass Feedback
- **Status**: Não implementado
- **Necessário para**:
  - `PassFeedback#` (acesso ao output do pass anterior no frame anterior)
  - Efeitos temporais complexos

### LUTs (Look-Up Tables)
- **Status**: Não implementado
- **Necessário para**:
  - Color grading avançado
  - Paletas personalizadas

### Texturas de Referência
- **Status**: Não implementado
- **Necessário para**:
  - Mask textures
  - Grain textures
  - Padrões customizados

## 📊 Conversões Implementadas

| Sintaxe Slang/Vulkan | GLSL 3.30 | Status |
|----------------------|-----------|--------|
| `#version 450` | `#version 330` | ✅ |
| `layout(push_constant)` | `uniform` | ✅ |
| `params.X` | `X` | ✅ |
| `global.MVP` | `Position` | ✅ |
| `global.X` | `X` | ✅ |
| `set =`, `binding =` | *(removido)* | ✅ |
| `layout(location =)` | *(removido)* + `glBindAttribLocation` | ✅ |
| `#pragma stage` | Separação vertex/fragment | ✅ |
| `#pragma parameter` | `uniform float` | ✅ |
| `uniform uint` | `uniform float` | ✅ |
| UBO parameters | `uniform float` | ✅ |
| `#include` | Processamento recursivo | ✅ |

## 🎯 Próximas Melhorias

### Alta Prioridade
1. **Implementar History Buffers**
   - Manter ringbuffer de N frames anteriores
   - Texturas `OriginalHistory0-7`
   - Valores reais para `OriginalHistorySize#`

2. **LUT Support**
   - Carregar texturas PNG/BMP
   - Binding correto de texturas adicionais
   - `User#` samplers

### Média Prioridade
3. **Pass Feedback**
   - `PassFeedback#` textures
   - Framebuffer persistence entre frames

4. **Alias Support**
   - Nomear passes com aliases
   - Referências cruzadas entre passes

### Baixa Prioridade
5. **Float Framebuffers**
   - Melhor suporte a HDR
   - Precision modes

6. **Mipmap Support**
   - `mipmap_input` directive
   - Automatic mipmap generation

## 💡 Dicas de Uso

### Para máximo compatibilidade:
1. Use shaders single-pass ou com poucos passes
2. Evite shaders que dependem de histórico de frames
3. Use presets da pasta `crt/shaders/` (mais simples)

### Para melhor performance:
1. Reduza resolução (`--width`, `--height`)
2. Use shaders sem blur/bloom passes
3. Desabilite float framebuffers quando possível

## 🐛 Problemas Resolvidos

### v0.3.0
- ✅ `RESSWITCH_GLITCH_TRESHOLD` undeclared
- ✅ `OriginalHistorySize1` undeclared
- ✅ `AS`, `asat` undeclared
- ✅ `internal_res` undeclared
- ✅ `FrameCount` undeclared em alguns shaders
- ✅ Parâmetros do UBO não sendo extraídos
- ✅ `#pragma parameter` não sendo processados

## 📝 Notas Técnicas

### Extração de Parâmetros
O sistema agora extrai parâmetros de três fontes:
1. **`push_constant` blocks**: Extraídos e convertidos para uniforms
2. **UBO blocks**: Parâmetros customizados extraídos (exceto builtins)
3. **`#pragma parameter`**: Todos os parâmetros declarados

### Ordem de Processamento
1. `#include` (recursivo)
2. Extração de `#pragma parameter`
3. Conversão de `push_constant`
4. Extração de parâmetros UBO
5. Remoção de blocos UBO
6. Substituição de `params.X` e `global.X`
7. Separação de vertex/fragment stages
8. Adição de uniforms faltantes

### Valores Padrão
Todos os parâmetros têm valores padrão seguros baseados nos shaders do RetroArch. Isso garante que shaders compilem mesmo sem configuração explícita.

