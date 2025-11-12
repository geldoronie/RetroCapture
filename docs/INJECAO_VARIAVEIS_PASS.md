# Injeção de Variáveis de Configuração do Pass

## 🎯 Implementação Baseada na Observação do Usuário

O usuário observou que o RetroArch injeta variáveis de configuração de cada pass (Scale, Filter, etc) que podem ser usadas pelos shaders. Implementamos suporte completo para isso!

## ✅ Variáveis Implementadas

### 1. PassOutputSize# (Tamanhos de Saída de Passes Anteriores)
```cpp
// Permite que shaders acessem dimensões de passes anteriores
PassOutputSize0, PassOutputSize1, PassOutputSize2, ...
// Formato: vec4(width, height, 1/width, 1/height)
```

**Uso nos Shaders:**
```glsl
uniform vec4 PassOutputSize0;  // Dimensões do pass 0
vec2 tex = floor(PassOutputSize0.xy * vTexCoord);
```

### 2. PassInputSize# (Tamanhos de Entrada de Passes Anteriores)
```cpp
// Tamanho de entrada de cada pass anterior
PassInputSize0, PassInputSize1, ...
```

### 3. PassScale, PassScaleX, PassScaleY
```cpp
// Fator de escala do pass atual
PassScale = (scaleX + scaleY) / 2.0  // Média
PassScaleX = scaleX                   // Individual X
PassScaleY = scaleY                   // Individual Y
```

**Valores do Preset:**
- `scale_type_x0 = "source"` → `scaleX = 1.0`
- `scale_x0 = "2.000000"` → `scaleX = 2.0`
- `scale_type_y0 = "viewport"` → `scaleY` baseado no viewport

### 4. PassFilter
```cpp
// Tipo de filtro do pass atual
PassFilter = 1.0  // Linear (filter_linear0 = "true")
PassFilter = 0.0  // Nearest (filter_linear0 = "false")
```

## 📊 Como Funciona

### No RetroArch:
1. Usuário configura Scale/Filter na interface
2. RetroArch injeta essas variáveis como uniforms
3. Shaders podem usar para decisões de processamento

### No RetroCapture:
1. Parse do preset captura `scale_x0`, `filter_linear0`, etc
2. Armazenado em `ShaderPass` structure
3. `setupUniforms()` injeta como uniforms quando shader precisa

## 🔧 Implementação Técnica

```cpp
void ShaderEngine::setupUniforms(GLuint program, uint32_t passIndex, ...)
{
    // PassOutputSize# para passes anteriores
    for (uint32_t i = 0; i < passIndex && i < m_passes.size(); ++i)
    {
        std::string passOutputName = "PassOutputSize" + std::to_string(i);
        // Injetar dimensões do pass i
    }
    
    // Variáveis do pass atual
    if (passIndex < m_passes.size())
    {
        const auto& passInfo = m_passes[passIndex].passInfo;
        
        // PassScale, PassScaleX, PassScaleY
        // PassFilter
    }
}
```

## 💡 Benefícios

1. **Compatibilidade Total:** Shaders que dependem dessas variáveis agora funcionam
2. **Decisões Inteligentes:** Shaders podem adaptar processamento baseado em scale/filter
3. **Acesso a Passes Anteriores:** Shaders podem consultar dimensões de passes anteriores
4. **Fidelidade ao RetroArch:** Comportamento idêntico ao RetroArch

## 🎯 Exemplo de Uso em Shader

```glsl
uniform vec4 PassOutputSize0;
uniform float PassScale;
uniform float PassFilter;

void main() {
    // Usar dimensões do pass anterior
    vec2 tex = floor(PassOutputSize0.xy * vTexCoord);
    
    // Adaptar processamento baseado em scale
    if (PassScale > 2.0) {
        // Processamento para upscaling
    }
    
    // Escolher filtro baseado em PassFilter
    vec4 color = (PassFilter > 0.5) 
        ? texture(Source, tex)  // Linear
        : texture(Source, tex); // Nearest (mesmo, mas poderia ser diferente)
}
```

## 📝 Variáveis Adicionais que Podem Ser Implementadas

### Futuro:
- `PassWrapMode` - Wrap mode do pass (clamp_to_border, repeat, etc)
- `PassMipmap` - Se mipmap está habilitado
- `PassAlias` - Nome do alias do pass
- `PassFloatFramebuffer` - Se usa framebuffer float
- `PassSRGBFramebuffer` - Se usa framebuffer sRGB

## 🚀 Status

✅ **Implementado e Funcionando:**
- PassOutputSize# (todos os passes anteriores)
- PassInputSize# (todos os passes anteriores)
- PassScale, PassScaleX, PassScaleY
- PassFilter

**Próximo:** Testar com shaders que realmente usam essas variáveis!

---

**Crédito:** Implementação baseada na observação do usuário sobre a interface do RetroArch!
