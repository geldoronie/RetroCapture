# RetroCapture v0.3.1 - Suporte a Shaders Complexos

## ✅ Problemas Resolvidos

### 1. `RESSWITCH_GLITCH_TRESHOLD` undeclared
**Causa**: Parâmetro do bloco UBO não estava sendo extraído  
**Solução**: Implementado extração de parâmetros do UBO, ignorando apenas builtins do RetroArch

### 2. `OriginalHistorySize1` undeclared
**Causa**: Texturas de histórico (frames anteriores) não eram suportadas  
**Solução**: Adicionados uniforms dummy para `OriginalHistorySize0-7` com dimensões do frame atual

### 3. `AS`, `asat` undeclared
**Causa**: Parâmetros declarados no UBO em linha única (`float AS, asat;`) não eram capturados  
**Solução**: Melhorada regex de extração para suportar múltiplos parâmetros por linha

### 4. `FrameCount` undeclared
**Causa**: `FrameCount` estava sendo ignorado como builtin mas não era sempre declarado  
**Solução**: `FrameCount` agora é SEMPRE declarado como uniform float

### 5. `internal_res` undeclared
**Causa**: Parâmetros de `#pragma parameter` não eram processados  
**Solução**: Implementado extração completa de todos os `#pragma parameter`

### 6. `RESSWITCH_ENABLE` redeclared
**Causa**: Parâmetro estava sendo declarado duas vezes (UBO + #pragma)  
**Solução**: Unificação de todos os parâmetros customizados em um único set antes de declarar

## 🔧 Melhorias Implementadas

### Extração de Parâmetros
```cpp
// 1. Extrair de #pragma parameter
std::regex pragmaParamRegex(R"(#pragma\s+parameter\s+(\w+))");
pragmaParams.insert(paramName);

// 2. Extrair de UBO
std::regex uboRegex(R"(layout\s*\([^)]*\)\s*uniform\s+UBO\s*\{([^}]*)\}\s*global\s*;)");
std::regex paramRegex(R"((float|uint|int)\s+(\w+)(?:\s*,\s*(\w+))*\s*;)");
uboParams.insert(paramName);

// 3. Unificar (evitando duplicatas)
std::set<std::string> allCustomParams;
allCustomParams.insert(customParams.begin(), customParams.end());
allCustomParams.insert(uboParams.begin(), uboParams.end());
allCustomParams.insert(pragmaParams.begin(), pragmaParams.end());
```

### Ordem de Processamento
1. ✅ `#include` recursivo
2. ✅ Extração de `#pragma parameter`
3. ✅ Remoção de `#pragma parameter`
4. ✅ Conversão `#version 450` → `#version 330`
5. ✅ Conversão `push_constant` → uniforms
6. ✅ Extração de parâmetros UBO
7. ✅ Remoção de bloco UBO
8. ✅ Substituição `global.X` → `X`
9. ✅ Substituição `params.X` → `X`
10. ✅ Separação vertex/fragment
11. ✅ Adição de uniforms faltantes

### Valores Padrão Adicionados
```cpp
// Afterglow/Grade
AS = 0.20f          // Afterglow Strength
asat = 0.33f        // Afterglow saturation
PR, PG, PB = 0.32f  // Persistence RGB

// Resolution Switch Glitch
RESSWITCH_ENABLE = 1.0f
RESSWITCH_GLITCH_TRESHOLD = 0.1f
RESSWITCH_GLITCH_BAR_STR = 0.6f
// ... mais parâmetros

// Resolução
internal_res = 1.0f
auto_res = 0.0f

// History buffers (dummy)
OriginalHistorySize0-7 = (inputWidth, inputHeight, 1/inputWidth, 1/inputHeight)
```

## 📊 Resultados

### Shader Testado
```bash
./build/bin/retrocapture --device /dev/video2 --preset \
  ./shaders/1080p/05-1080p-crt-guest-advanced-hd-aperture-grille-u-normal-rgb.slangp
```

**Passes**: 12  
**Status**: ✅ **Todos os 12 passes compilados com sucesso!**

### Passes do Preset
1. ✅ `stock.slang` - Passthrough
2. ✅ `crt-resswitch-glitch-koko.slang` - Resolution switch glitch
3. ✅ `afterglow0.slang` - Phosphor afterglow
4. ✅ `pre-shaders-afterglow-grade.slang` - Color grading
5. ✅ `linearize-hd.slang` - Linearization
6. ✅ `crt-guest-advanced-hd-pass1.slang` - CRT emulation pass 1
7. ✅ `gaussian_horizontal.slang` - Gaussian blur H
8. ✅ `gaussian_vertical.slang` - Gaussian blur V
9. ✅ `bloom_horizontal.slang` - Bloom H
10. ✅ `bloom_vertical.slang` - Bloom V
11. ✅ `crt-guest-advanced-hd-pass2.slang` - CRT emulation pass 2
12. ✅ `deconvergence-hd.slang` - Deconvergence

## 🐛 Limitações

### History Buffers
- **Status**: Declarados mas com valores dummy
- **Impacto**: Efeitos de afterglow não mostram frames anteriores reais
- **Workaround**: Usa dimensões do frame atual

### Pass Feedback
- **Status**: Não implementado
- **Impacto**: Não pode acessar output do pass anterior no frame anterior

### LUTs
- **Status**: Não implementado
- **Impacto**: Shaders que usam texturas de referência não funcionam completamente

## 🎯 Próximos Passos

1. **Implementar History Buffers reais**
   - Ring buffer de frames anteriores
   - Texturas `OriginalHistory0-7` funcionais

2. **Implementar LUT support**
   - Carregar PNG/BMP
   - Binding de texturas adicionais

3. **Implementar Pass Feedback**
   - Persistência de framebuffers entre frames

## 📝 Arquivo Modificado

**`src/shader/ShaderEngine.cpp`**:
- Extração de `#pragma parameter`
- Extração melhorada de parâmetros UBO (suporta múltiplos por linha)
- Unificação de parâmetros customizados (sem duplicatas)
- Valores padrão para todos os parâmetros comuns
- History buffer uniforms (dummy)
- `FrameCount` sempre declarado

## 🚀 Performance

**Teste**: Preset de 12 passes @ 1080p60  
**Dispositivo**: /dev/video2  
**Status**: Compilação bem-sucedida, pronto para renderização

**Nota**: Alguns efeitos visuais podem não aparecer corretamente devido às limitações de history buffers e LUTs, mas o shader não causará mais erros de compilação.

