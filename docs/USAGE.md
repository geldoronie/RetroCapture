# Como Usar o RetroCapture

## Especificar Shader/Preset

### Usar um Preset (Múltiplos Passes)

```bash
./build/bin/retrocapture --preset shaders/1080p/01-1080p-crt-guest-advanced-hd-aperture-grille-u-normal-rf.slangp
```

### Usar um Shader Simples

```bash
./build/bin/retrocapture --shader shaders/test.glsl
```

### Especificar Dispositivo de Captura

```bash
# Usar /dev/video1 em vez de /dev/video0
./build/bin/retrocapture --device /dev/video1 --preset shaders/1080p/05-1080p-crt-guest-advanced-hd-aperture-grille-u-normal-rgb.slangp
```

### Ver Ajuda

```bash
./build/bin/retrocapture --help
```

## Exemplos Completos

```bash
# Preset CRT com efeitos avançados
./build/bin/retrocapture --preset shaders/1080p/01-1080p-crt-guest-advanced-hd-aperture-grille-u-normal-rf.slangp

# Preset RGB (sem processamento NTSC)
./build/bin/retrocapture --preset shaders/1080p/05-1080p-crt-guest-advanced-hd-aperture-grille-u-normal-rgb.slangp

# Shader simples de teste
./build/bin/retrocapture --shader shaders/test.glsl

# Sem shader (captura direta)
./build/bin/retrocapture
```

## Notas

- Se não especificar shader/preset, a captura será exibida sem efeitos
- Os presets na pasta `shaders/1080p/` são compatíveis com o RetroArch
- Os caminhos podem ser relativos ao diretório de execução ou absolutos

