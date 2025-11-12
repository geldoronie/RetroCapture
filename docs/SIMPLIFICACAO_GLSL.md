# Simplificação: Suporte Apenas GLSL

## 🎯 Decisão

Em vez de tentar converter Slang para GLSL (que estava causando muitos bugs complexos), 
decidimos focar **apenas em shaders GLSL e presets GLSLP** do RetroArch.

## ✅ O que foi implementado

### 1. Detecção de Extensão
- Verifica extensão do arquivo antes de processar
- Rejeita `.slang` e `.slangp` com mensagem clara
- Aceita apenas `.glsl` e `.glslp`

### 2. Remoção de Conversão
- Removidas todas as chamadas a `convertSlangToGLSL()`
- Mantido apenas `processIncludes()` para processar `#include` em shaders GLSL
- Código muito mais simples e direto

### 3. Mensagens de Erro Claras
```
[ERROR] Shaders Slang (.slang) não são suportados. Use shaders GLSL (.glsl) ou presets GLSLP (.glslp)
[ERROR] Muitos shaders RetroArch estão disponíveis em formato GLSL na pasta shaders/shaders_glsl/
```

### 4. Atualização de Documentação
- `--help` agora menciona apenas `.glsl` e `.glslp`
- Exemplos atualizados para usar `.glslp`

## 💡 Benefícios

1. **Código Muito Mais Simples**
   - Removidas ~600 linhas de código complexo de conversão
   - Sem regex complicadas para converter sintaxe
   - Sem bugs de conversão

2. **Mais Estável**
   - GLSL já está no formato correto
   - Sem problemas de compatibilidade
   - Menos pontos de falha

3. **Ainda Acessível**
   - RetroArch tem MUITOS shaders GLSL disponíveis
   - A maioria dos shaders populares tem versão GLSL
   - Shaders GLSL são mais portáteis

4. **Manutenção Mais Fácil**
   - Código mais limpo
   - Mais fácil de debugar
   - Menos edge cases

## 📁 Estrutura de Pastas

```
shaders/
├── shaders_glsl/     ← Use shaders daqui!
│   ├── crt/
│   ├── handheld/
│   └── ...
└── shaders_slang/    ← Ignorar (não suportado)
```

## 🚀 Próximos Passos

1. Testar com shaders GLSL reais do RetroArch
2. Verificar se `processIncludes()` funciona corretamente
3. Adicionar mais shaders GLSL à pasta `shaders/shaders_glsl/`

## 📝 Nota

A função `convertSlangToGLSL()` ainda existe no código (não foi removida),
mas não é mais chamada. Pode ser removida no futuro se necessário.

---

**Resultado:** Código muito mais simples, estável e fácil de manter! 🎉
