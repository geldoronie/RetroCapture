# Status do Bug do Swizzle - Investigação Completa

## 🔍 O QUE DESCOBRIMOS

### 1. O Erro
```
0:89(15): error: cannot access field `xxx' of non-structure / non-vector
0:99(14): error: cannot access field `xxx' of non-structure / non-vector  
0:100(14): error: cannot access field `xxx' of non-structure / non-vector
```

### 2. Shader Original (Válido)
```glsl
vec3 color = 0.0.xxx;   // Linha 127
vec3 cmax = 0.0.xxx;    // Linha 137
vec3 cmin = 1.0.xxx;    // Linha 138
```

**Sintaxe GLSL Válida:** `0.0.xxx` cria `vec3(0.0, 0.0, 0.0)` usando swizzle.

### 3. Apenas `internal_res` é Variável Local
```
[INFO] Processando variável local com prefixo: internal_res
```

**NÃO** há `xxx` como variável local ou parâmetro!

### 4. Parâmetros do Pass 5
```
HARNG, HSHARP, HSHARPNESS, MAXS, SIGMA_HOR, SIGMA_VER, S_SHARP, 
VSHARPNESS, auto_res, internal_res, spike
```

**NÃO** inclui `xxx`!

### 5. Substituição de `params.X`
```cpp
result = std::regex_replace(result, std::regex(R"(params\.(\w+))"), "$1");
```

Isso **NÃO deveria** afetar `0.0.xxx` porque procura literalmente `params.` antes.

### 6. Filtramos Swizzles
Adicionamos filtro para remover swizzles da lista de parâmetros:
```cpp
std::set<std::string> glslSwizzles = {"xxx", "xxxx", "rgb", "rgba", ...};
```

**Mas o erro persiste!**

## 🤔 POSSIBILIDADES RESTANTES

### Hipótese 1: Linhas Deslocadas
- O erro está nas linhas 89, 99, 100 do shader CONVERTIDO
- No shader ORIGINAL, estão nas linhas 127, 137, 138
- Pode haver código adicional inserido que desloca as linhas

### Hipótese 2: Algo ANTES Corrompe o Código
- A substituição de `params.X` pode estar afetando algo indireto
- Ex: `params.OriginalSize.xxx` → `OriginalSize.xxx` (válido) mas depois...?

### Hipótese 3: Incompatibilidade GLSL 3.30
- Talvez OpenGL 3.3 não suporte `0.0.xxx` como GLSL 4.50?
- Improvável, mas possível

### Hipótese 4: Bug no Compilador GLSL
- O driver pode ter um bug específico com swizzles numéricos
- Improvável, mas worth checking

## 🎯 PRÓXIMOS PASSOS

### Passo 1: Ver o Shader Convertido
Precisamos ver exatamente o que está nas linhas 89, 99, 100 do shader convertido!

### Passo 2: Testar Sintaxe Diretamente
Criar um shader minimal test:
```glsl
#version 330
void main() {
    vec3 test = 0.0.xxx;
    gl_FragColor = vec4(test, 1.0);
}
```

### Passo 3: Converter Swizzles Numéricos
Se necessário, converter `0.0.xxx` para `vec3(0.0)` durante a conversão:
```cpp
result = std::regex_replace(result, std::regex(R"((\d+\.\d+)\.xxx)"), "vec3($1)");
result = std::regex_replace(result, std::regex(R"((\d+\.\d+)\.xxxx)"), "vec4($1)");
```

## 💡 INSIGHT IMPORTANTE

Este NÃO é um problema de:
- ❌ Parâmetros sendo substituídos incorretamente
- ❌ Variáveis locais conflitantes
- ❌ Uniforms sendo adicionados como swizzles

É provavelmente:
- ✅ Incompatibilidade de sintaxe entre GLSL 4.50 e 3.30
- ✅ Ou algo sutil na conversão que estamos perdendo

## 🚀 SOLUÇÃO PROPOSTA

**Converter swizzles numéricos para construção explícita:**

```cpp
// Converter swizzles numéricos para construção vec explícita
// 0.0.xxx → vec3(0.0)
// 1.0.xxx → vec3(1.0)
// 0.5.xxxx → vec4(0.5)
result = std::regex_replace(result, std::regex(R"((\d+\.\d+)\.xxx\b)"), "vec3($1)");
result = std::regex_replace(result, std::regex(R"((\d+\.\d+)\.xxxx\b)"), "vec4($1)");
result = std::regex_replace(result, std::regex(R"((\d+\.\d+)\.yyy\b)"), "vec3($1)");
result = std::regex_replace(result, std::regex(R"((\d+\.\d+)\.yyyy\b)"), "vec4($1)");
```

Isso é uma conversão segura e explícita que funciona em qualquer versão GLSL!
