# Exemplos de Uso

## Captura Simples (Sem Shader)

```bash
# Resolução padrão (1920x1080 @ 60fps)
./build/bin/retrocapture --device /dev/video2

# Resolução 720p @ 30fps
./build/bin/retrocapture --device /dev/video2 --width 1280 --height 720 --fps 30

# Resolução 4K @ 30fps
./build/bin/retrocapture --device /dev/video2 --width 3840 --height 2160 --fps 30
```

## Com Shader Simples

```bash
./build/bin/retrocapture --device /dev/video2 --shader shaders/test.glsl
```

## Com Preset CRT (Single-Pass)

```bash
./build/bin/retrocapture --device /dev/video2 --preset shaders/shaders_slang/crt/zfast-crt.slangp
```

## Com Preset Complexo (Multi-Pass)

```bash
# CRT Guest Advanced HD
./build/bin/retrocapture --device /dev/video2 --preset ./shaders/1080p/01-1080p-crt-guest-advanced-hd-aperture-grille-u-normal-rf.slangp

# Outros presets da pasta 1080p
./build/bin/retrocapture --device /dev/video2 --preset ./shaders/1080p/05-1080p-crt-guest-advanced-hd-aperture-grille-u-normal-rgb.slangp
```

## Combinando Opções

```bash
# Usar /dev/video1 com 720p e CRT shader
./build/bin/retrocapture --device /dev/video1 --width 1280 --height 720 --fps 30 --preset shaders/shaders_slang/crt/zfast-crt.slangp

# 4K com shader complexo (requer GPU potente)
./build/bin/retrocapture --device /dev/video2 --width 3840 --height 2160 --fps 30 --preset ./shaders/1080p/01-1080p-crt-guest-advanced-hd-aperture-grille-u-normal-rf.slangp
```

## Listando Dispositivos e Resoluções Disponíveis

```bash
# Ver dispositivos V4L2
v4l2-ctl --list-devices

# Ver formatos e resoluções suportadas
v4l2-ctl --device /dev/video2 --list-formats-ext

# Ver informações do dispositivo
v4l2-ctl --device /dev/video2 --all
```

## Resoluções Comuns

| Resolução | Largura | Altura | FPS Recomendado |
|-----------|---------|--------|-----------------|
| 480p      | 720     | 480    | 30/60           |
| 720p      | 1280    | 720    | 30/60           |
| 1080p     | 1920    | 1080   | 30/60           |
| 1440p     | 2560    | 1440   | 30/60           |
| 4K        | 3840    | 2160   | 30              |

**Nota:** Verifique as resoluções suportadas pelo seu dispositivo de captura antes de usar.

## Shaders Disponíveis

### CRT Shaders
- `shaders/shaders_slang/crt/zfast-crt.slangp` - CRT rápido e simples
- `shaders/shaders_slang/crt/crt-geom.slangp` - CRT geométrico com curvatura
- `shaders/shaders_slang/crt/crt-royale.slangp` - CRT de alta qualidade

### Presets 1080p
Vários presets otimizados para 1080p na pasta `shaders/1080p/`

### Custom Shaders
Coloque seus shaders `.glsl` ou `.slang` na pasta `shaders/` e use:
```bash
./build/bin/retrocapture --device /dev/video2 --shader shaders/meu_shader.glsl
```

## Ajuda

```bash
./build/bin/retrocapture --help
```
