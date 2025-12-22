# Otimizações de Desempenho - RetroCapture

Este documento lista oportunidades de otimização identificadas no código, além das resoluções configuráveis já implementadas.

## Prioridade Alta (Impacto Significativo)

### 1. ✅ Reutilização de Buffer RGB na Conversão YUYV - **IMPLEMENTADO**
**Localização**: `src/processing/FrameProcessor.cpp:147`

**Problema**: 
- A cada frame, um novo `std::vector<uint8_t> rgbBuffer` é alocado
- Para 1920x1080, isso são ~6MB alocados/liberados a cada frame (60fps = 360MB/s de alocações)

**Solução**:
- ✅ Adicionar membro `std::vector<uint8_t> m_rgbBuffer` na classe `FrameProcessor`
- ✅ Redimensionar apenas quando necessário (quando dimensões mudam)
- ✅ Reutilizar o mesmo buffer entre frames

**Impacto Estimado**: Redução de 5-10% no uso de CPU, eliminação de pausas por alocação

**Status**: Implementado no commit `4100470`

---

### 2. ✅ Otimização da Conversão YUYV→RGB com SIMD/NEON - **IMPLEMENTADO**
**Localização**: `src/processing/FrameProcessor.cpp:202-276`

**Problema**:
- Conversão YUYV→RGB está em CPU pura, processando pixel por pixel
- No ARM (Raspberry Pi), poderia usar instruções NEON para processar múltiplos pixels simultaneamente

**Solução**:
- ✅ Implementar versão NEON da conversão (usa NEON para carregamento otimizado)
- ✅ Manter versão fallback para sistemas sem NEON
- ✅ Usar `#ifdef __ARM_NEON` para compilação condicional
- ✅ Usar exatamente as mesmas fórmulas da versão escalar para garantir correção

**Impacto Estimado**: Melhoria no carregamento de dados (benefício parcial do NEON)

**Status**: Implementado - versão funcional usando NEON para carregamento, cálculos idênticos à versão escalar para garantir correção. Pode ser otimizado futuramente para usar NEON nos cálculos também.

---

### 3. ✅ Reutilização de Framebuffer Temporário no Motion Blur - **IMPLEMENTADO**
**Localização**: `src/shader/ShaderEngine.cpp:1518-1573`

**Problema**:
- A cada frame com motion blur, um framebuffer temporário é criado (`glGenFramebuffers`) e deletado (`glDeleteFramebuffers`)
- Operações de criação/deleção de framebuffers são custosas

**Solução**:
- ✅ Adicionar membro `GLuint m_copyFramebuffer = 0` na classe `ShaderEngine`
- ✅ Criar uma vez quando necessário, reutilizar entre frames
- ✅ Deletar apenas no cleanup (shutdown)

**Impacto Estimado**: Redução de overhead de OpenGL, especialmente em shaders com motion blur

**Status**: Implementado no commit `4100470`

---

### 4. Otimização de glReadPixels com PBO (Pixel Buffer Objects) ✅
**Localização**: `src/core/Application.cpp:2707` - **IMPLEMENTADO**

**Problema**:
- `glReadPixels` é síncrono e bloqueia o pipeline OpenGL
- Para streaming, isso pode causar stuttering

**Solução**:
- ✅ Criar classe `PBOManager` para gerenciar PBOs com double-buffering
- ✅ Implementar 2 PBOs (enquanto um lê, o outro processa)
- ✅ Leitura assíncrona: `glReadPixels` escreve no PBO sem bloquear
- ✅ Alternar PBOs a cada frame para máxima eficiência
- ✅ Fallback para `glReadPixels` síncrono se PBO não está disponível

**Impacto Estimado**: Redução de stuttering no streaming, melhor uso do pipeline OpenGL

**Status**: Implementado - `PBOManager` criado e integrado em `Application`

---

## Prioridade Média (Impacto Moderado)

### 5. Cache de Uniform Locations Melhorado
**Localização**: `src/shader/ShaderEngine.cpp` (múltiplas chamadas a `getUniformLocation`)

**Problema**:
- `getUniformLocation` é chamado várias vezes por frame para os mesmos uniforms
- Já existe cache (`m_uniformLocations`), mas pode ser otimizado

**Solução**:
- Verificar se o cache está sendo usado corretamente em todos os lugares
- Adicionar cache por shader program (não apenas por nome)
- Pré-cachear todos os uniforms conhecidos após compilação do shader

**Impacto Estimado**: Redução pequena mas constante de overhead

---

### 6. Redução de Chamadas OpenGL Desnecessárias
**Localização**: Múltiplos arquivos

**Problema**:
- Múltiplas chamadas `glBindTexture`, `glActiveTexture`, etc. que poderiam ser agrupadas
- Estados OpenGL sendo alterados mesmo quando já estão corretos

**Solução**:
- Implementar state tracking (verificar estado atual antes de alterar)
- Agrupar chamadas relacionadas
- Usar `glBindTextureUnit` (OpenGL 4.5+) ao invés de `glActiveTexture` + `glBindTexture`

**Impacto Estimado**: Redução de overhead de estado OpenGL

---

### 7. Otimização de Texture Filtering
**Localização**: `src/processing/FrameProcessor.cpp:98-99`, `src/shader/ShaderEngine.cpp:2591-2592`

**Problema**:
- `GL_LINEAR` é usado por padrão, mas pode ser mais lento que `GL_NEAREST`
- Para imagens pixel-perfect (retro), `GL_NEAREST` pode ser suficiente e mais rápido

**Solução**:
- Adicionar opção configurável para filtering (Linear vs Nearest)
- Usar `GL_NEAREST` por padrão para texturas de captura (pixel-perfect)
- Usar `GL_LINEAR` apenas quando necessário (shaders que fazem blur, etc.)

**Impacto Estimado**: Redução pequena mas constante, especialmente em GPUs integradas

---

### 8. Threading: Processamento Paralelo de Frames
**Localização**: `src/core/Application.cpp` (loop principal)

**Problema**:
- Captura, conversão YUYV→RGB, e upload de textura acontecem sequencialmente na thread principal
- Enquanto processa um frame, não captura o próximo

**Solução**:
- Implementar double/triple buffering de frames
- Thread de captura: apenas captura frames e coloca em fila
- Thread de processamento: converte YUYV→RGB e faz upload de textura
- Thread principal: apenas renderiza textura já pronta

**Impacto Estimado**: Redução de latência, melhor uso de múltiplos cores (especialmente em ARM multi-core)

**Complexidade**: Alta - requer sincronização cuidadosa

---

## Prioridade Baixa (Impacto Pequeno ou Específico)

### 9. VSync e Frame Pacing
**Localização**: `src/output/WindowManagerSDL.cpp`

**Problema**:
- Verificar se VSync está habilitado corretamente
- Frame pacing pode ser melhorado para evitar tearing

**Solução**:
- Garantir que `SDL_GL_SetSwapInterval(1)` está sendo chamado
- Implementar frame pacing customizado se necessário

**Impacto Estimado**: Melhor experiência visual, menos tearing

---

### 10. Otimização de Memory Allocations
**Localização**: Múltiplos arquivos

**Problema**:
- Verificar se há outras alocações frequentes que poderiam ser reutilizadas

**Solução**:
- Usar memory pools para objetos temporários
- Reutilizar buffers sempre que possível

**Impacto Estimado**: Redução geral de overhead de memória

---

## Métricas para Avaliar Melhorias

### Antes de Implementar:
1. Medir FPS atual com shader ativo
2. Medir uso de CPU (%)
3. Medir uso de GPU (%)
4. Medir latência de frame (captura → exibição)

### Após Implementar:
1. Comparar FPS
2. Comparar uso de CPU/GPU
3. Verificar se latência melhorou
4. Verificar se há regressões visuais

---

## Ordem Recomendada de Implementação

1. ✅ **Reutilização de Buffer RGB** (Fácil, alto impacto) - **IMPLEMENTADO**
2. ✅ **Reutilização de Framebuffer Temporário** (Fácil, médio impacto) - **IMPLEMENTADO**
3. ✅ **Otimização YUYV→RGB com NEON** (Médio, alto impacto em ARM) - **IMPLEMENTADO**
4. **PBO para glReadPixels** (Médio, alto impacto no streaming)
5. **Cache de Uniforms melhorado** ✅ (Fácil, baixo impacto) - **IMPLEMENTADO**
   - Função `preCacheCommonUniforms()` adicionada para pré-cachear uniforms comuns após linkagem
   - Chamada automaticamente após compilação bem-sucedida de shaders (modo simples e preset)
   - Reduz chamadas `glGetUniformLocation` repetidas para uniforms frequentes

6. **Texture Filtering configurável** ✅ (Fácil, baixo impacto) - **IMPLEMENTADO**
   - Adicionado `m_textureFilterLinear` em `Application` (padrão: false = GL_NEAREST)
   - Método `setTextureFilterLinear()` em `FrameProcessor` para configurar filtering
   - GL_NEAREST por padrão (mais rápido para pixel-perfect, adequado para retro)
   - GL_LINEAR disponível quando necessário (melhor qualidade para imagens suaves)

7. **State tracking OpenGL** ✅ (Médio, baixo impacto) - **IMPLEMENTADO**
   - Classe `OpenGLStateTracker` criada para rastrear estados OpenGL
   - Evita chamadas `glBindTexture` e `glActiveTexture` desnecessárias
   - Integrado em `OpenGLRenderer` para otimizar bind de texturas
   - Suporta até 32 unidades de textura (GL_TEXTURE0 a GL_TEXTURE31)
8. **Threading** (Difícil, alto impacto mas complexo)

---

## Notas Adicionais

- **Perfilamento**: Usar ferramentas como `perf` (Linux) ou `gprof` para identificar gargalos reais
- **ARM Específico**: Muitas otimizações (NEON, threading) terão maior impacto em ARM
- **Testes**: Sempre testar em hardware real (Raspberry Pi) após otimizações
- **Compatibilidade**: Manter fallbacks para sistemas sem suporte a otimizações específicas

