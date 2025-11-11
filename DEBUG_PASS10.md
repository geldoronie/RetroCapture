# Debug do Pass 10 - Erro Linha 295

## Erro
```
0:295(12): error: syntax error, unexpected FLOATCONSTANT, expecting ',' or ';'
```

## Linha 295 Original
```glsl
float wsum = 0.0;
```

**Isso não tem nada a ver com swizzles!**

## Linhas ao Redor (Original)
```glsl
290: tex.y = floor(Size.y * tex.y)*Size.w + 0.5*Size.w;
291: vec3 color = 0.0.xxx;
292: vec2 dy  = vec2(0.0, Size.w);
293: (vazio)
294: float w = 0.0;
295: float wsum = 0.0;  ← ERRO AQUI
296: vec3 pixel;
```

## Possíveis Causas

### 1. Conversão de Swizzles Afetando Algo Antes
- Linha 291: `vec3 color = 0.0.xxx;` → `vec3 color = vec3(0.0);` ✅ OK
- Mas pode haver algo sutil...

### 2. Conversão de params.X Afetando Algo
- Se `params.Size` foi convertido incorretamente
- Ou se `Size.w` está sendo afetado

### 3. Problema na Separação de Stages
- A separação vertex/fragment pode estar removendo algo importante
- Ou adicionando algo inválido

### 4. Erro em Cascata
- Erro real está antes da linha 295
- Compilador reporta na linha 295 porque é onde percebe o problema

## Próximos Passos

1. Salvar shader ANTES de qualquer processamento
2. Salvar shader DEPOIS de cada etapa de conversão
3. Verificar se há algum problema com `Size.w` ou `0.5*Size.w`
4. Testar se GLSL 3.30 realmente suporta swizzles numéricos

## Hipótese Principal

O problema pode ser que `0.70.xxx` em expressões como `pow(color1, 0.70.xxx-0.325*sat)` não pode ser convertido diretamente porque cria expressões inválidas. Mas o erro está na linha 295, não na 468, então pode ser um erro em cascata.

Vamos tentar: **NÃO converter swizzles em expressões aritméticas**, deixar como está e ver se GLSL 3.30 aceita.
