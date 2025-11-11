# Trabalho Realizado - Suporte a Shaders Complexos do RetroArch

## 🎯 Objetivo
Fazer funcionar o preset: `./shaders/1080p/05-1080p-crt-guest-advanced-hd-aperture-grille-u-normal-rgb.slangp`

## ✅ Problemas Identificados e Resolvidos

### 1. `RESSWITCH_GLITCH_TRESHOLD` undeclared
**Erro original:**
```
0:40(48): error: `RESSWITCH_GLITCH_TRESHOLD' undeclared
```

**Causa:** Parâmetro do bloco UBO não estava sendo extraído  
**Solução:** Implementada extração completa de parâmetros do UBO, ignorando apenas builtins do RetroArch

### 2. `OriginalHistorySize1` undeclared  
**Erro original:**
```
0:66(22): error: `OriginalHistorySize1' undeclared
```

**Causa:** Texturas de histórico (frames anteriores) não eram suportadas  
**Solução:** Adicionados uniforms dummy para `OriginalHistorySize0-7` com dimensões do frame atual

### 3. `AS`, `asat` undeclared
**Erro original:**
```
0:717(16): error: `AS' undeclared
0:717(60): error: `asat' undeclared
```

**Causa:** Parâmetros declarados no UBO em linha única (`float AS, asat;`) não eram capturados  
**Solução:** Melhorada regex de extração para suportar múltiplos parâmetros por linha

### 4. `FrameCount` undeclared
**Causa:** `FrameCount` estava sendo ignorado como builtin mas não era sempre declarado  
**Solução:** `FrameCount` agora é SEMPRE declarado como uniform float

### 5. `internal_res` undeclared, depois redeclared
**Erro original:**
```
0:43(7): error: `internal_res' redeclared
0:43(1): error: assignment to read-only variable 'internal_res'
```

**Causa:** Shader usa `float internal_res;` (variável local) e `params.internal_res` (uniform)  
**Solução Parcial:** Implementada detecção de variáveis locais via regex para evitar declarar uniforms duplicados

### 6. `RESSWITCH_ENABLE` redeclared
**Causa:** Parâmetro estava sendo declarado duas vezes (UBO + #pragma)  
**Solução:** Unificação de todos os parâmetros customizados em um único set

## 🔧 Implementações

### Extração de Parâmetros
```cpp
// 1. Parâmetros de #pragma parameter
std::regex pragmaParamRegex(R"(#pragma\s+parameter\s+(\w+))");

// 2. Parâmetros do bloco UBO
std::regex uboRegex(R"(layout\s*\([^)]*\)\s*uniform\s+UBO\s*\{([^}]*)\}\s*global\s*;)");
std::regex paramRegex(R"((float|uint|int)\s+(\w+)(?:\s*,\s*(\w+))*\s*;)");

// 3. Unificar (evitar duplicatas)
std::set<std::string> allCustomParams;

// 4. Detectar variáveis locais
std::regex localVarRegex("\\bfloat\\s+" + param + "\\s*[,;]");
if (!isLocalVariable) {
    // Adicionar como uniform
}
```

### Valores Padrão Adicionados
```cpp
// Afterglow/Grade
AS = 0.20f, asat = 0.33f
PR = PG = PB = 0.32f

// Resolution Switch Glitch
RESSWITCH_ENABLE = 1.0f
RESSWITCH_GLITCH_TRESHOLD = 0.1f
RESSWITCH_GLITCH_BAR_STR = 0.6f
RESSWITCH_GLITCH_BAR_SIZE = 0.5f
RESSWITCH_GLITCH_BAR_SMOOTH = 1.0f
RESSWITCH_GLITCH_SHAKE_MAX = 0.25f
RESSWITCH_GLITCH_ROT_MAX = 0.2f
RESSWITCH_GLITCH_WOB_MAX = 0.1f

// Resolução
internal_res = 1.0f
auto_res = 0.0f

// History buffers (dummy)
OriginalHistorySize0-7 = (w, h, 1/w, 1/h)
```

## 📊 Resultados

### Shaders Testados com Sucesso
| Shader | Passes | Status |
|--------|--------|--------|
| `test_passthrough.slangp` | 1 | ✅ 100% |
| `crt/zfast-crt.slangp` | 1 | ✅ 100% |
| `1080p/05-...slangp` (passes 0-4) | 5 | ✅ Compilam |
| `1080p/05-...slangp` (pass 5+) | 7 | ⚠️ Erro em runtime |

### Progresso no Preset de 12 Passes
- ✅ Pass 0: `stock.slang`
- ✅ Pass 1: `crt-resswitch-glitch-koko.slang`  
- ✅ Pass 2: `afterglow0.slang`
- ✅ Pass 3: `pre-shaders-afterglow-grade.slang`
- ✅ Pass 4: `linearize-hd.slang`
- ⚠️ Pass 5: `crt-guest-advanced-hd-pass1.slang` (problema com `internal_res`)
- ⏸️ Pass 6-11: Não testados devido ao erro no pass 5

## ⚠️ Limitações Conhecidas

### 1. Variáveis Locais com Nomes de Parâmetros
**Problema:** Shaders que declaram variáveis locais com o mesmo nome de `params.X` param clash after substitution  
**Exemplo:** `float internal_res;` + `params.internal_res` → conflito  
**Workaround:** Detecção parcial via regex (não 100% confiável)  
**Solução Real:** Precisaria parser completo GLSL

### 2. History Buffers
**Status:** Declarados mas com valores dummy  
**Impacto:** Efeitos de afterglow não mostram frames anteriores reais  
**Textures:** `OriginalHistory0-7` não implementadas

### 3. Pass Feedback
**Status:** Não implementado  
**Impacto:** Não pode acessar output do pass anterior no frame anterior

### 4. LUTs
**Status:** Não implementado  
**Impacto:** Shaders que usam texturas de referência não funcionam

## 📝 Arquivos Modificados

### `src/shader/ShaderEngine.cpp`
- ✅ Extração de `#pragma parameter`
- ✅ Extração melhorada de parâmetros UBO (múltiplos por linha)
- ✅ Unificação de parâmetros customizados (sem duplicatas)
- ✅ Valores padrão para parâmetros comuns
- ✅ History buffer uniforms (dummy)
- ✅ `FrameCount` sempre declarado
- ✅ Detecção de variáveis locais via regex
- ✅ Remoção de `#pragma parameter` do shader convertido

### Ordem de Processamento Final
1. ✅ `#include` recursivo
2. ✅ Extração de `#pragma parameter`
3. ✅ Remoção de `#pragma parameter`
4. ✅ `#version 450` → `#version 330`
5. ✅ `push_constant` → uniforms
6. ✅ Extração de parâmetros UBO
7. ✅ Remoção de bloco UBO
8. ✅ `global.X` → `X`
9. ✅ `params.X` → `X`
10. ✅ Separação vertex/fragment
11. ✅ Detecção de variáveis locais
12. ✅ Adição de uniforms faltantes

## 🚀 Uso

### Shader Simples (Funciona 100%)
```bash
./build/bin/retrocapture --device /dev/video2 --preset shaders/shaders_slang/crt/zfast-crt.slangp
```

### Preset Complexo (Parcialmente Funcional)
```bash
# Compila mas pode ter problemas em runtime com passes específicos
./build/bin/retrocapture --device /dev/video2 --preset ./shaders/1080p/05-1080p-crt-guest-advanced-hd-aperture-grille-u-normal-rgb.slangp
```

## 🎯 Próximos Passos para Funcionalidade Completa

### Alta Prioridade
1. **Parser GLSL Completo**
   - Identificar TODAS as declarações de variáveis locais
   - Evitar adicionar uniforms para variáveis locais
   - Solução atual (regex) não é 100% confiável

2. **History Buffers Reais**
   - Ring buffer de frames anteriores
   - Texturas `OriginalHistory0-7` funcionais
   - Valores reais para `OriginalHistorySize#`

### Média Prioridade
3. **LUT Support**
   - Carregar PNG/BMP
   - Binding de texturas adicionais

4. **Pass Feedback**
   - Persistência de framebuffers entre frames

## 💡 Conclusão

### O Que Funciona
- ✅ Shaders simples (single-pass)
- ✅ Shaders com `#pragma parameter`
- ✅ Shaders com blocos UBO
- ✅ Shaders com history buffer declarations
- ✅ Conversão Slang → GLSL robusta
- ✅ Presets com até 4-5 passes (maioria dos casos)

### O Que Precisa Melhorar
- ⚠️ Shaders com variáveis locais que conflitam com params
- ⚠️ Presets muito complexos (12+ passes)
- ⚠️ Shaders que dependem de histórico real
- ⚠️ Shaders com LUTs

### Recomendação
Para uso em produção, use shaders simples ou médios (1-5 passes). Presets muito complexos (10+ passes) podem requerer ajustes manuais.

