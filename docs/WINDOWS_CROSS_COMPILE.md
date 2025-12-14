# Compilação Cross-Platform: Windows a partir do Linux

> **Nota:** Para informações sobre o status atual da implementação Windows, consulte [STATUS_WINDOWS_PORT.md](./STATUS_WINDOWS_PORT.md).

Este documento descreve as alternativas disponíveis para compilar o RetroCapture para Windows a partir de um ambiente Linux.

## 📋 Alternativas Disponíveis

### 1. MinGW-w64 (Recomendado) ⭐

**MinGW-w64** é a solução mais comum e recomendada para cross-compilation de C++ para Windows a partir do Linux.

#### Vantagens:

- ✅ Nativo do Linux, não requer emulação
- ✅ Suporta CMake nativamente
- ✅ Gera executáveis Windows nativos (.exe)
- ✅ Boa compatibilidade com bibliotecas
- ✅ Atualizado e mantido ativamente

#### Desvantagens:

- ⚠️ Algumas bibliotecas podem precisar ser compiladas separadamente
- ⚠️ Media Foundation pode ter limitações (mas funciona)

#### Instalação (Arch/Manjaro):

```bash
sudo pacman -S mingw-w64-gcc mingw-w64-cmake mingw-w64-crt
```

#### Instalação (Ubuntu/Debian):

```bash
sudo apt-get install mingw-w64 g++-mingw-w64 cmake
```

#### Uso com CMake:

```bash
mkdir build-windows
cd build-windows
cmake .. -DCMAKE_SYSTEM_NAME=Windows \
         -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
         -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
         -DCMAKE_FIND_ROOT_PATH=/usr/x86_64-w64-mingw32
make
```

### 2. MXE (M cross environment)

**MXE** é um ambiente de compilação cruzada que facilita a compilação de muitas bibliotecas para Windows.

#### Vantagens:

- ✅ Facilita compilação de dependências (FFmpeg, OpenSSL, etc.)
- ✅ Scripts automatizados para bibliotecas comuns
- ✅ Suporta CMake

#### Desvantagens:

- ⚠️ Requer compilar dependências (pode demorar)
- ⚠️ Mais complexo de configurar inicialmente

#### Instalação:

```bash
git clone https://github.com/mxe/mxe.git
cd mxe
make MXE_TARGETS=x86_64-w64-mingw32.shared \
     ffmpeg openssl glfw3 libpng
```

#### Uso:

```bash
export PATH=$(pwd)/mxe/usr/bin:$PATH
mkdir build-windows
cd build-windows
cmake .. -DCMAKE_TOOLCHAIN_FILE=../mxe/usr/x86_64-w64-mingw32.shared/share/cmake/mxe-conf.cmake
make
```

### 3. Docker com Ambiente Windows

Usar Docker para criar um ambiente de compilação Windows isolado.

#### Vantagens:

- ✅ Ambiente isolado e reproduzível
- ✅ Pode usar Visual Studio Build Tools
- ✅ Fácil de compartilhar com outros desenvolvedores

#### Desvantagens:

- ⚠️ Requer Docker
- ⚠️ Imagens podem ser grandes
- ⚠️ Mais lento que compilação nativa

#### Exemplo Dockerfile:

```dockerfile
FROM mcr.microsoft.com/windows/servercore:ltsc2022
# Instalar Visual Studio Build Tools
RUN ...
```

### 4. Wine + Visual Studio Build Tools

Usar Wine para executar ferramentas de compilação Windows no Linux.

#### Vantagens:

- ✅ Acesso a ferramentas nativas do Windows
- ✅ Melhor suporte a Media Foundation

#### Desvantagens:

- ⚠️ Wine pode ter problemas de compatibilidade
- ⚠️ Mais complexo de configurar
- ⚠️ Pode ser instável

### 5. GitHub Actions / CI/CD

Usar serviços de CI/CD para compilar automaticamente no Windows.

#### Vantagens:

- ✅ Compilação automática em cada commit
- ✅ Ambiente Windows real
- ✅ Não requer configuração local

#### Desvantagens:

- ⚠️ Requer acesso à internet
- ⚠️ Não permite debug local

#### Exemplo GitHub Actions:

```yaml
name: Build Windows
on: [push, pull_request]
jobs:
  build:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v2
      - name: Build
        run: |
          cmake -B build
          cmake --build build
```

## 🎯 Recomendação: MinGW-w64

Para o RetroCapture, recomendamos **MinGW-w64** porque:

1. É a solução mais simples e direta
2. Suporta todas as bibliotecas que usamos (GLFW, FFmpeg, OpenSSL)
3. CMake tem suporte nativo
4. Gera executáveis Windows funcionais

## 🔧 Configuração do CMake para Cross-Compilation

Vamos criar um arquivo de toolchain para facilitar a compilação:

### Arquivo: `cmake/toolchain-mingw-w64.cmake`

```cmake
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Compiladores
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)

# Root do sistema
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)

# Buscar programas, bibliotecas e headers no sistema host
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
```

### Uso:

```bash
mkdir build-windows
cd build-windows
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-mingw-w64.cmake
make
```

## 📦 Dependências para Windows (via vcpkg ou pré-compiladas)

### Opção A: vcpkg (no Windows)

Se você tiver acesso a uma máquina Windows, pode usar vcpkg:

```bash
vcpkg install glfw3:x64-windows
vcpkg install ffmpeg:x64-windows
vcpkg install openssl:x64-windows
vcpkg install libpng:x64-windows
```

### Opção B: Bibliotecas pré-compiladas MinGW-w64

Algumas bibliotecas podem ser instaladas via pacotes:

```bash
# Arch/Manjaro
sudo pacman -S mingw-w64-ffmpeg mingw-w64-openssl mingw-w64-libpng

# Ubuntu/Debian
sudo apt-get install libffmpeg-mingw-w64-dev libssl-mingw-w64-dev
```

### Opção C: Compilar dependências com MXE

Use MXE para compilar todas as dependências de uma vez.

## 🚀 Script de Build Automatizado

Podemos criar um script `build-windows.sh` para automatizar o processo:

```bash
#!/bin/bash
set -e

echo "=== Building RetroCapture for Windows ==="

# Verificar se MinGW-w64 está instalado
if ! command -v x86_64-w64-mingw32-g++ &> /dev/null; then
    echo "Erro: MinGW-w64 não encontrado"
    echo "Instale com: sudo pacman -S mingw-w64-gcc (Arch) ou sudo apt-get install mingw-w64 (Ubuntu)"
    exit 1
fi

# Criar diretório de build
BUILD_DIR="build-windows"
mkdir -p $BUILD_DIR
cd $BUILD_DIR

# Configurar CMake
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-mingw-w64.cmake \
    -DCMAKE_BUILD_TYPE=Release

# Compilar
make -j$(nproc)

echo "=== Build concluído! ==="
echo "Executável: $BUILD_DIR/bin/retrocapture.exe"
```

## ⚠️ Limitações Conhecidas

### Media Foundation

- ✅ **Resolvido:** O RetroCapture agora usa carregamento dinâmico de funções do Media Foundation (`GetProcAddress`), tornando-o compatível com MinGW/MXE
- ✅ `MFEnumDeviceSources` é carregado dinamicamente para evitar problemas de linkagem
- ⚠️ Alguns controles de hardware podem não estar disponíveis (limitação do Media Foundation)
- ⚠️ Testes em Windows real são recomendados para validação completa

### Bibliotecas Nativas do Windows

- Algumas APIs do Windows podem não estar disponíveis
- Testar em ambiente Windows real é recomendado

## 📝 Próximos Passos

1. **Criar toolchain file** para MinGW-w64
2. **Criar script de build** automatizado
3. **Testar compilação** com dependências básicas
4. **Configurar CI/CD** (GitHub Actions) para builds automáticos
5. **Testar executável** em ambiente Windows real

## 🔗 Referências

- [MinGW-w64](https://www.mingw-w64.org/)
- [MXE](https://mxe.cc/)
- [CMake Cross Compiling](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html#cross-compiling)
- [vcpkg](https://github.com/Microsoft/vcpkg)
