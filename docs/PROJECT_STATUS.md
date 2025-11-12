# RetroCapture - Status do Projeto

## Versão Atual: 0.3.0

### ✅ Funcionalidades Implementadas

#### 1. Sistema de Captura V4L2
- [x] Abertura e configuração de dispositivos de captura
- [x] Memory mapping para buffers de vídeo
- [x] Captura em tempo real com baixa latência
- [x] Conversão YUYV para RGB
- [x] Descarte de frames antigos (sempre processa o frame mais recente)
- [x] **Resolução configurável via linha de comando**
- [x] **Framerate configurável via linha de comando**
- [x] **Validação de capacidades do dispositivo**

#### 2. Sistema de Renderização OpenGL
- [x] Contexto OpenGL 3.3+ via GLFW
- [x] Carregamento dinâmico de funções OpenGL
- [x] Renderização de texturas em tempo real
- [x] Gerenciamento de VAO/VBO/EBO
- [x] Correção automática de orientação Y

#### 3. Sistema de Shaders
- [x] Suporte a shaders GLSL (.glsl)
- [x] Suporte a shaders Slang (.slang)
- [x] Carregamento de presets RetroArch (.slangp, .glslp)
- [x] **Conversão automática Slang → GLSL**
- [x] Múltiplos passes de shader com framebuffers intermediários
- [x] Processamento de diretivas `#include`
- [x] Resolução de paths complexos do RetroArch
- [x] Uniforms do RetroArch (SourceSize, OutputSize, FrameCount, etc.)
- [x] Parâmetros customizados de shaders

#### 4. Interface de Linha de Comando
- [x] Argumentos para especificar shader/preset
- [x] Argumento para especificar dispositivo (`--device`)
- [x] **Argumento para resolução (`--width`, `--height`)**
- [x] **Argumento para framerate (`--fps`)**
- [x] Sistema de ajuda (`--help`)

#### 5. Utilitários
- [x] Sistema de logging
- [x] Tratamento de erros
- [x] Validação de parâmetros
- [x] Detecção de capacidades do hardware

### 📊 Conversões Slang → GLSL Implementadas

| Conversão | Status | Descrição |
|-----------|--------|-----------|
| `#version 450` → `#version 330` | ✅ | Compatibilidade com OpenGL 3.3 |
| `layout(push_constant)` → `uniform` | ✅ | Conversão de uniforms Vulkan |
| Remoção de `set =`, `binding =` | ✅ | Limpa layouts não suportados |
| Remoção de `layout(location =)` | ✅ | Compatibilidade GLSL 3.3 |
| `params.X` → `X` | ✅ | Substituição de referências |
| `uniform uint` → `uniform float` | ✅ | Tipos suportados |
| `#pragma stage` | ✅ | Separação vertex/fragment |
| `#include` recursivo | ✅ | Resolução de dependências |
| `global.MVP` → `Position` | ✅ | Simplificação de transformações |
| UBO removal | ✅ | Remove uniform blocks |

### 📈 Resoluções Suportadas

| Resolução | Status | Observações |
|-----------|--------|-------------|
| 480p (720x480) | ✅ | Testado |
| 720p (1280x720) | ✅ | Testado |
| 1080p (1920x1080) | ✅ | Padrão, testado |
| 1440p (2560x1440) | ✅ | Requer GPU moderna |
| 4K (3840x2160) | ✅ | Requer GPU potente |
| Customizado | ✅ | 1-7680 x 1-4320 |

### 🎮 Shaders Testados

| Shader | Status | Performance |
|--------|--------|-------------|
| Passthrough (test) | ✅ | Excelente |
| zfast-crt | ✅ | Ótima |
| CRT Guest Advanced HD | ✅ | Boa (1080p) |
| Presets 1080p | ✅ | Variável |

### 📝 Documentação

| Documento | Status | Descrição |
|-----------|--------|-----------|
| README.md | ✅ | Visão geral |
| QUICK_START.md | ✅ | Guia rápido |
| PARAMETERS.md | ✅ | Referência completa |
| RUN_EXAMPLES.md | ✅ | Exemplos de uso |
| CHANGELOG.md | ✅ | Histórico |
| USAGE.md | ✅ | Uso básico |
| PLANEJAMENTO.md | ✅ | Arquitetura |

## 🚀 Próximas Funcionalidades

### Fase 4 - Interface e Controles (Planejado)

- [ ] Hotkeys (ESC para sair, F5 para recarregar shader, etc.)
- [ ] OSD (On-Screen Display) para informações
- [ ] Menu de configuração em tempo real
- [ ] Ajuste de parâmetros de shader via UI
- [ ] Troca de shaders sem reiniciar

### Fase 5 - Recursos Avançados (Planejado)

- [ ] Suporte a LUTs (Look-Up Tables)
- [ ] Texturas de referência para shaders
- [ ] Gravação de vídeo
- [ ] Screenshots
- [ ] Cache de shaders compilados
- [ ] Profiles/Presets salvos
- [ ] Suporte a múltiplos dispositivos simultâneos

### Fase 6 - Otimização (Planejado)

- [ ] Threading para captura e renderização
- [ ] Redução de latência adicional
- [ ] Suporte a hardware encoding
- [ ] Profiles de performance
- [ ] Benchmarking integrado

## 🐛 Problemas Conhecidos

1. **Framerate não configurável em alguns dispositivos**
   - Status: Esperado - alguns dispositivos V4L2 não suportam
   - Workaround: Sistema detecta e usa framerate nativo

2. **Alguns shaders muito complexos podem ter performance ruim**
   - Status: Normal - shaders do RetroArch são pesados
   - Workaround: Reduzir resolução ou usar shaders mais simples

3. **Diretiva `layout(location)` removida pode causar problemas**
   - Status: Resolvido com `glBindAttribLocation`
   - Solução: Binding explícito antes de link

## 💡 Lições Aprendidas

1. **OpenGL Function Loading**: GLFW's `glfwGetProcAddress` é mais confiável que `dlsym`
2. **Texture Orientation**: Câmeras V4L2 geralmente têm Y invertido
3. **Shader Compatibility**: Slang (Vulkan) e GLSL (OpenGL) têm diferenças significativas
4. **V4L2 Capabilities**: Nem todos os dispositivos suportam todas as features
5. **Framebuffer Management**: Importante limpar e desbind corretamente

## 📊 Métricas

- **Linhas de Código**: ~3500
- **Arquivos Fonte**: 15
- **Tempo de Desenvolvimento**: 1 dia
- **Shaders Testados**: 5+
- **Resoluções Testadas**: 3 (720p, 1080p, 4K)

## 🎯 Maturidade do Projeto

| Aspecto | Status | Nota |
|---------|--------|------|
| Captura V4L2 | 🟢 Estável | Funcional e testado |
| Renderização | 🟢 Estável | Sem problemas conhecidos |
| Sistema de Shaders | 🟡 Beta | Funcional, mas pode ter edge cases |
| Conversão Slang→GLSL | 🟡 Beta | Cobre casos comuns |
| Documentação | 🟢 Completa | Guias e exemplos |
| Testes | 🟡 Básico | Testado manualmente |

**Legenda:**
- 🟢 Estável / Completo
- 🟡 Beta / Em desenvolvimento
- 🔴 Instável / Incompleto

## 🏆 Conclusão

O projeto atingiu seus objetivos iniciais de criar um software de captura de vídeo Linux com suporte a shaders RetroArch em tempo real. O sistema é funcional, documentado e pronto para uso.

**Próximo Marco:** Implementar interface de controle em tempo real e hotkeys.
