# Guia Rápido - RetroCapture

## Instalação Rápida

```bash
# 1. Instalar dependências (Arch/Manjaro)
sudo pacman -S cmake gcc glfw libv4l pkg-config

# 2. Compilar
./build.sh

# 3. Verificar dispositivo
v4l2-ctl --list-devices
```

## Primeiros Passos

### 1. Teste Básico (Sem Shader)

```bash
# Usar padrão (1920x1080 @ 60fps)
./build/bin/retrocapture --device /dev/video2
```

### 2. Adicionar um Shader CRT

```bash
# Shader CRT simples e rápido
./build/bin/retrocapture --device /dev/video2 --preset shaders/shaders_slang/crt/zfast-crt.slangp
```

### 3. Ajustar Performance

```bash
# Se estiver lento, reduza a resolução
./build/bin/retrocapture --device /dev/video2 --width 1280 --height 720 --fps 30 --preset shaders/shaders_slang/crt/zfast-crt.slangp
```

## Comandos Mais Usados

```bash
# Ver dispositivos disponíveis
v4l2-ctl --list-devices

# Ver resoluções suportadas
v4l2-ctl --device /dev/video2 --list-formats-ext

# Ver ajuda completa
./build/bin/retrocapture --help
```

## Resoluções Recomendadas por Hardware

### GPU Fraca/Integrada
```bash
./build/bin/retrocapture --device /dev/video2 --width 1280 --height 720 --fps 30
```

### GPU Média (GTX 1060 / RX 580)
```bash
./build/bin/retrocapture --device /dev/video2 --width 1920 --height 1080 --fps 60
```

### GPU Potente (RTX 3070+ / RX 6800+)
```bash
./build/bin/retrocapture --device /dev/video2 --width 3840 --height 2160 --fps 30
```

## Resolvendo Problemas Comuns

### Erro: "Falha ao abrir dispositivo"
- Verifique se o dispositivo está conectado
- Confirme o path correto: `v4l2-ctl --list-devices`
- Verifique permissões: `ls -l /dev/video*`

### Erro: "Falha ao configurar formato"
- A resolução pode não ser suportada
- Liste as suportadas: `v4l2-ctl --device /dev/video2 --list-formats-ext`
- Tente uma resolução da lista

### Tela Preta com Shader
- Teste sem shader primeiro
- Verifique se o arquivo do shader existe
- Veja os logs de erro no console

### Performance Ruim
1. Reduza a resolução: use 720p ao invés de 1080p
2. Reduza o FPS: use 30fps ao invés de 60fps
3. Use shaders mais simples: `zfast-crt.slangp` é o mais leve

## Documentação Completa

- `README.md` - Visão geral do projeto
- `PARAMETERS.md` - Referência completa de parâmetros
- `RUN_EXAMPLES.md` - Mais exemplos de uso
- `CHANGELOG.md` - Histórico de mudanças

## Atalhos do Teclado

⚠️ **Em desenvolvimento** - Atualmente use `Ctrl+C` no terminal para sair.

Planejado para futuras versões:
- `ESC` - Sair
- `F1` - Alternar shader on/off
- `F5` - Recarregar shader
- `+/-` - Ajustar brilho

