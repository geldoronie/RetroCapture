# Configuração do MXE para Compilação Cross-Platform

## O que é MXE?

**MXE (M cross environment)** é um ambiente de compilação cruzada que facilita a compilação de bibliotecas C/C++ para Windows a partir do Linux. Ele automatiza o processo de compilar dependências como GLFW, FFmpeg, OpenSSL, etc.

## Por que usar MXE?

- ✅ **Automatiza compilação de dependências** - Não precisa compilar cada biblioteca manualmente
- ✅ **Gerenciamento de versões** - Garante compatibilidade entre bibliotecas
- ✅ **Suporte a CMake** - Integração fácil com nosso projeto
- ✅ **Compilação otimizada** - Bibliotecas compiladas especificamente para Windows

## Como Usar

### Opção 1: Script Automatizado (Recomendado)

```bash
./setup-mxe.sh
```

Este script irá:
1. Clonar o MXE (se não existir)
2. Fazer bootstrap do MXE
3. Compilar todas as dependências necessárias (GLFW, FFmpeg, OpenSSL, libpng)
4. Verificar se tudo foi compilado corretamente

**Tempo estimado:** 30-60 minutos (depende do hardware)

### Opção 2: Manual

```bash
# 1. Clonar MXE
git clone https://github.com/mxe/mxe.git
cd mxe

# 2. Fazer bootstrap (compilar compilador base)
make -j$(nproc)

# 3. Compilar dependências
make -j$(nproc) glfw3 ffmpeg openssl libpng MXE_TARGETS=x86_64-w64-mingw32.shared
```

## Dependências do Sistema

O script `setup-mxe.sh` **instala automaticamente** as dependências necessárias:

- **git** - Para clonar o MXE
- **make** - Para compilar
- **gcc/g++** - Compilador C++
- **lzip** - Para descompactar alguns pacotes
- **gperf** - Gerador de hash perfeito
- **intltool** - Ferramentas de internacionalização
- **ruby** - Linguagem de script
- **python-mako** - Template engine (mako-render)
- **Espaço em disco** - MXE requer ~5-10GB

### Instalação Manual (se necessário)

Se preferir instalar manualmente:

**Arch/Manjaro:**
```bash
sudo pacman -S git make gcc lzip gperf intltool ruby python-mako
```

**Ubuntu/Debian:**
```bash
sudo apt-get install git make build-essential lzip gperf intltool ruby python3-mako
```

**Fedora:**
```bash
sudo dnf install git make gcc-c++ lzip gperf intltool ruby python3-mako
```

## Uso com build-windows.sh

Após configurar o MXE, o `build-windows.sh` detectará automaticamente e perguntará se deseja usar:

```bash
./build-windows.sh
# O script detectará o MXE e perguntará se deseja usar
```

Ou force o uso do MXE:
```bash
# Se MXE estiver na raiz do projeto
./build-windows.sh

# Se MXE estiver em outro lugar
export MXE_DIR=/caminho/para/mxe
./build-windows.sh
```

## Variáveis de Ambiente

Você pode customizar o comportamento do MXE:

```bash
# Diretório do MXE (padrão: ./mxe)
export MXE_DIR=/caminho/para/mxe

# Target (padrão: x86_64-w64-mingw32.shared)
export MXE_TARGET=x86_64-w64-mingw32.shared

# Jobs paralelos (padrão: número de CPUs)
export PARALLEL_JOBS=8
```

## Troubleshooting

### Erro: python-setuptools / easy_install

**Problema:** O MXE pode falhar ao compilar `python-setuptools` com erro:
```
error: invalid command 'easy_install'
```

**Solução:** O script `setup-mxe.sh` agora lida com isso automaticamente:
- Se o erro for apenas de `python-setuptools`, o bootstrap continua
- O compilador será criado mesmo assim
- `python-setuptools` não é necessário para GLFW, FFmpeg, OpenSSL e libpng

Se quiser instalar manualmente:
```bash
# Arch/Manjaro
sudo pacman -S python-setuptools

# Ubuntu/Debian
sudo apt-get install python3-setuptools
```

### Erro: llvm falha no bootstrap

**Problema:** O MXE pode falhar ao compilar `llvm` durante o bootstrap:
```
Failed to build package llvm for target x86_64-pc-linux-gnu!
```

**Solução:** O script `setup-mxe.sh` agora lida com isso:
- `llvm` não é necessário para compilar GLFW, FFmpeg, OpenSSL e libpng
- O script verifica se algum compilador MinGW foi criado
- Se sim, tenta compilar as dependências diretamente
- O MXE pode criar o compilador necessário durante a compilação das dependências

**Nota:** Se o bootstrap falhar completamente, você pode tentar:
```bash
# Com menos jobs paralelos (mais estável)
PARALLEL_JOBS=4 ./setup-mxe.sh

# Ou compilar dependências diretamente (força criação do compilador)
cd mxe
make glfw3 MXE_TARGETS=x86_64-w64-mingw32.shared
```

### Erro: FFmpeg falha devido a 'lame'

**Problema:** O FFmpeg pode falhar ao compilar porque a dependência opcional `lame` (codificação MP3) falhou:
```
Failed to build package lame for target x86_64-w64-mingw32.shared!
configure.in:425: error: possibly undefined macro: AM_ICONV
```

**Solução:** O script `setup-mxe.sh` tenta automaticamente:
1. Se o FFmpeg falhar, o script detecta o problema
2. Aplica um patch temporário para remover `lame` das dependências do FFmpeg
3. Tenta compilar o FFmpeg novamente sem `lame`
4. Restaura o arquivo original após a tentativa

**Nota:** O `lame` é uma dependência **opcional** do FFmpeg. O FFmpeg funcionará perfeitamente sem ele, apenas não terá suporte a codificação MP3 (que não é necessário para o RetroCapture).

**Solução manual (se o script automático falhar):**
```bash
cd mxe
# Editar src/ffmpeg.mk e remover 'lame' da linha $(PKG)_DEPS
sed -i 's/ lame / /g' src/ffmpeg.mk
sed -i '/--enable-libmp3lame/d' src/ffmpeg.mk
make ffmpeg MXE_TARGETS=x86_64-w64-mingw32.shared
```

### MXE não compila

1. **Verificar espaço em disco:**
   ```bash
   df -h
   ```
   Certifique-se de ter pelo menos 10GB livres.

2. **Verificar dependências do sistema:**
   ```bash
   which git make g++
   ```

3. **Ver logs de erro:**
   ```bash
   cd mxe
   make <pacote> MXE_TARGETS=x86_64-w64-mingw32.shared 2>&1 | tee build.log
   ```

4. **Verificar se o compilador foi criado:**
   ```bash
   ls mxe/usr/x86_64-w64-mingw32.shared/bin/*-gcc
   ```
   Se o compilador existe, o bootstrap foi bem-sucedido mesmo com erros de python-setuptools.

### Dependências não encontradas

Se o `build-windows.sh` não encontrar as bibliotecas compiladas pelo MXE:

1. Verificar se MXE foi compilado corretamente:
   ```bash
   ls mxe/usr/x86_64-w64-mingw32.shared/lib/
   ```

2. Verificar se o caminho está correto no script

### Compilação muito lenta

- Reduzir jobs paralelos: `PARALLEL_JOBS=2 ./setup-mxe.sh`
- Compilar apenas dependências necessárias
- Usar SSD em vez de HDD

## Alternativas ao MXE

Se MXE não funcionar para você:

1. **Compilar manualmente cada biblioteca**
2. **Usar bibliotecas pré-compiladas** (se disponíveis)
3. **Usar GitHub Actions** para compilar em ambiente Windows real

## Referências

- [MXE Website](https://mxe.cc/)
- [MXE GitHub](https://github.com/mxe/mxe)
- [MXE Packages](https://mxe.cc/#packages)

