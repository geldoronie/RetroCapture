# Parâmetros de Configuração

## Parâmetros de Linha de Comando

### Opções de Shader

#### `--shader <caminho>`
Carrega um shader GLSL simples (.glsl)

**Exemplo:**
```bash
./build/bin/retrocapture --shader shaders/test.glsl
```

#### `--preset <caminho>`
Carrega um preset com múltiplos passes (.glslp, .slangp)

**Exemplo:**
```bash
./build/bin/retrocapture --preset shaders/shaders_slang/crt/zfast-crt.slangp
```

### Opções de Captura

#### `--device <caminho>`
Especifica o dispositivo de captura V4L2 a ser usado.

**Padrão:** `/dev/video0`

**Exemplo:**
```bash
./build/bin/retrocapture --device /dev/video2
```

**Como descobrir seu dispositivo:**
```bash
v4l2-ctl --list-devices
```

#### `--width <valor>`
Define a largura (em pixels) da captura.

**Padrão:** `1920`  
**Limites:** 1 a 7680 pixels  
**Resoluções comuns:** 720, 1280, 1920, 2560, 3840

**Exemplo:**
```bash
./build/bin/retrocapture --width 1280
```

#### `--height <valor>`
Define a altura (em pixels) da captura.

**Padrão:** `1080`  
**Limites:** 1 a 4320 pixels  
**Resoluções comuns:** 480, 720, 1080, 1440, 2160

**Exemplo:**
```bash
./build/bin/retrocapture --height 720
```

#### `--fps <valor>`
Define o framerate (quadros por segundo) da captura.

**Padrão:** `60`  
**Limites:** 1 a 240 fps  
**Valores comuns:** 24, 30, 60, 120, 144

**Exemplo:**
```bash
./build/bin/retrocapture --fps 30
```

**Nota:** Nem todos os dispositivos suportam configuração de framerate. Se não for possível configurar, o dispositivo usará seu framerate padrão.

### Outras Opções

#### `--help` ou `-h`
Mostra a ajuda com todos os parâmetros disponíveis.

```bash
./build/bin/retrocapture --help
```

## Exemplos Completos

### Captura 720p @ 30fps com CRT shader
```bash
./build/bin/retrocapture --device /dev/video2 --width 1280 --height 720 --fps 30 --preset shaders/shaders_slang/crt/zfast-crt.slangp
```

### Captura 1080p @ 60fps (configuração padrão)
```bash
./build/bin/retrocapture --device /dev/video2 --preset shaders/shaders_slang/crt/zfast-crt.slangp
```

### Captura 4K @ 30fps sem shader
```bash
./build/bin/retrocapture --device /dev/video2 --width 3840 --height 2160 --fps 30
```

## Resolução de Problemas

### Erro: "Falha ao configurar formato de captura"
- Verifique se a resolução é suportada pelo dispositivo:
  ```bash
  v4l2-ctl --device /dev/video2 --list-formats-ext
  ```
- Tente uma resolução menor ou uma das resoluções listadas

### Aviso: "Não foi possível configurar framerate"
- Alguns dispositivos não permitem configuração de framerate
- O dispositivo usará seu framerate nativo
- Isso não é crítico e a captura continuará funcionando

### Erro: "Largura/Altura/FPS inválido"
- Verifique se os valores estão dentro dos limites:
  - Largura: 1-7680
  - Altura: 1-4320
  - FPS: 1-240

## Performance

### Resoluções e seus impactos:

| Resolução | Uso de Memória | Carga GPU | Recomendação |
|-----------|----------------|-----------|--------------|
| 720p      | Baixo          | Baixa     | Ótimo para testes |
| 1080p     | Médio          | Média     | Recomendado (padrão) |
| 1440p     | Alto           | Alta      | Requer GPU moderna |
| 4K        | Muito Alto     | Muito Alta | Requer GPU de alto desempenho |

### Dicas de Performance:

1. **Para testes rápidos:** Use 720p @ 30fps
2. **Para uso geral:** Use 1080p @ 60fps (padrão)
3. **Para qualidade máxima:** Use a resolução nativa do seu dispositivo
4. **Shaders complexos:** Reduza resolução ou fps se houver travamentos

