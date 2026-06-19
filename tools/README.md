# Scripts de Build e Ferramentas

Esta pasta contém todos os scripts bash para build, instalação e utilitários do RetroCapture.

## 📦 Scripts de Build

### Build Direto na Raspberry Pi

#### `build-on-raspberry-pi.sh`

Compila RetroCapture diretamente na Raspberry Pi (recomendado para compatibilidade).

```bash
./tools/build-on-raspberry-pi.sh [Release|Debug] [SDL2]
```

**Exemplos:**

```bash
# Build Release padrão (GLFW)
./tools/build-on-raspberry-pi.sh Release

# Build Release com SDL2 (DirectFB/framebuffer)
./tools/build-on-raspberry-pi.sh Release SDL2

# Build Debug
./tools/build-on-raspberry-pi.sh Debug SDL2
```

### Build via Docker (Cross-compilation)

#### `build-linux-arm64v8-docker.sh`

Compila para Linux ARM64v8 (Raspberry Pi 4/5) usando Docker.

```bash
./tools/build-linux-arm64v8-docker.sh [Release|Debug] [--rebuild] [SDL2]
```

**Exemplos:**

```bash
# Build Release padrão
./tools/build-linux-arm64v8-docker.sh Release

# Build Release com SDL2
./tools/build-linux-arm64v8-docker.sh Release SDL2

# Build com rebuild completo da imagem Docker
./tools/build-linux-arm64v8-docker.sh Release --rebuild SDL2
```

#### `build-linux-arm32v7-docker.sh`

Compila para Linux ARM32v7 (Raspberry Pi 3) usando Docker.

```bash
./tools/build-linux-arm32v7-docker.sh [Release|Debug] [--rebuild] [SDL2]
```

**Exemplos:**

```bash
# Build Release com SDL2
./tools/build-linux-arm32v7-docker.sh Release SDL2
```

#### `build-linux-x86_64-docker.sh`

Compila para Linux x86_64 usando Docker.

```bash
./tools/build-linux-x86_64-docker.sh [Release|Debug] [--rebuild]
```

#### `build-windows-x86_64-docker.sh`

Compila para Windows x86_64 usando Docker (MXE/MinGW).

```bash
./tools/build-windows-x86_64-docker.sh [Release|Debug] [--rebuild]
```

### Scripts de Distribuição

#### `build-windows-installer.sh`

Gera instalador Windows completo (NSIS) com todos os componentes.

```bash
./tools/build-windows-installer.sh
```

#### `build-linux-appimage-x86_64.sh`

Gera AppImage para Linux x86_64.

```bash
./tools/build-linux-appimage-x86_64.sh
```

## 🔧 Scripts de Instalação e Utilitários

#### `install-deps-raspberry-pi.sh`

Instala todas as dependências necessárias na Raspberry Pi.

```bash
./tools/install-deps-raspberry-pi.sh
```

#### `check-directfb.sh`

Verifica suporte a DirectFB no SDL2 instalado.

```bash
./tools/check-directfb.sh
```

#### `sync-source-raspiberry.sh`

Sincroniza código fonte para Raspberry Pi (utilitário de desenvolvimento).

**Primeira configuração:**

```bash
# Configurar parâmetros interativamente
./tools/sync-source-raspiberry.sh --config
```

**Uso:**

```bash
# Sincronização única
./tools/sync-source-raspiberry.sh --once

# Sincronização contínua (monitora mudanças)
./tools/sync-source-raspiberry.sh

# Com parâmetros específicos
./tools/sync-source-raspiberry.sh --ip 192.168.1.100 --user pi --dest /home/pi/Projects/RetroCapture
```

**Parâmetros:**

- `--ip IP`: IP ou hostname do servidor remoto
- `--user USER`: Usuário SSH
- `--port PORT`: Porta SSH (padrão: 22)
- `--source DIR`: Diretório fonte local (padrão: diretório atual)
- `--dest DIR`: Diretório destino remoto
- `--once`: Sincronização única (sem monitoramento)
- `--config`: Configurar parâmetros interativamente
- `--help`: Mostrar ajuda

**Autenticação SSH:**
O script configura automaticamente autenticação por chave SSH para evitar solicitar senha toda vez. Na primeira execução, ele oferece gerar e copiar a chave SSH automaticamente.

**Arquivo de configuração:**
`~/.retrocapture-sync-config` (criado automaticamente)

## 🐳 Scripts Docker (Internos)

Estes scripts são usados dentro dos containers Docker durante o build:

- `docker-build-linux-arm64v8.sh` - Build dentro do container ARM64v8
- `docker-build-linux-arm32v7.sh` - Build dentro do container ARM32v7
- `docker-build-linux-x86_64.sh` - Build dentro do container Linux x86_64
- `docker-build-windows-x86_64.sh` - Build dentro do container Windows

**Nota:** Estes scripts são chamados automaticamente pelos scripts Docker principais. Não é necessário executá-los manualmente.

## 🧪 Testes

### `smoke-test.sh`

Smoke-test ponta-a-ponta do pipeline de captura/stream/encode (#149). Sobe um
RetroCapture já compilado com a fonte sintética de teste (`--source test`, sem
hardware), habilita streaming, puxa `/stream`, decodifica um frame e valida que
o vídeo continua correto: presença de brilho, variância espacial, cor (barras
SMPTE saturadas), barras distintas e variância temporal (marcador móvel). É a
rede de segurança para refatorações — uma refatoração que preserva comportamento
deve mantê-lo verde.

```bash
# usa build-linux-x86_64/bin/retrocapture por padrão
./tools/smoke-test.sh

# ou aponte um binário específico
./tools/smoke-test.sh /caminho/para/retrocapture
```

Requisitos: `ffmpeg`, `ffprobe`, `curl`, `python3` e um display (usa `$DISPLAY`,
ou cai pra `xvfb-run` em CI). Usa a porta 8080 — recusa rodar se já houver uma
instância servindo nela (use `SMOKE_PORT` para mudar). Exit 0 = passou.

## 📝 Notas

- Todos os scripts de build suportam argumentos em qualquer ordem
- SDL2 pode ser passado como `SDL2`, `sdl2`, `ON` ou `on`
- Build type pode ser `Release` (padrão) ou `Debug`
- Scripts Docker suportam `--rebuild` para forçar rebuild completo da imagem

## 🎯 Uso Recomendado

### Para Raspberry Pi 4/5 (ARM64):

```bash
# Build direto na Raspberry (mais rápido, melhor compatibilidade)
./tools/build-on-raspberry-pi.sh Release SDL2

# Ou via Docker (cross-compilation de outro sistema)
./tools/build-linux-arm64v8-docker.sh Release SDL2
```

### Para Raspberry Pi 3 (ARM32v7):

```bash
# Build direto na Raspberry
./tools/build-on-raspberry-pi.sh Release SDL2

# Ou via Docker
./tools/build-linux-arm32v7-docker.sh Release SDL2
```

### Para Linux x86_64:

```bash
./tools/build-linux-x86_64-docker.sh Release
```

### Para Windows:

```bash
./tools/build-windows-x86_64-docker.sh Release
```
