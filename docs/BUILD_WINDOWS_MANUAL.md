# Compilação Manual para Windows - Guia Simples

Este guia mostra como compilar o RetroCapture para Windows de forma manual e direta.

## Opção 1: Usar MinGW-w64 Diretamente (Mais Simples)

### 1. Instalar MinGW-w64

**Arch/Manjaro:**
```bash
sudo pacman -S mingw-w64-gcc mingw-w64-cmake
```

**Ubuntu/Debian:**
```bash
sudo apt-get install mingw-w64 g++-mingw-w64 cmake
```

### 2. Instalar Dependências Windows

As bibliotecas precisam ser compiladas manualmente ou baixadas pré-compiladas:

- **GLFW**: https://www.glfw.org/download.html
- **FFmpeg**: https://www.gyan.dev/ffmpeg/builds/ (ou compilar)
- **OpenSSL**: https://slproweb.com/products/Win32OpenSSL.html
- **libpng**: Geralmente vem com outras bibliotecas

### 3. Compilar

```bash
mkdir build-windows
cd build-windows
cmake .. -DCMAKE_SYSTEM_NAME=Windows \
         -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
         -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
         -DCMAKE_FIND_ROOT_PATH=/usr/x86_64-w64-mingw32
make
```

## Opção 2: Compilar no Windows com Visual Studio

### 1. Instalar Visual Studio 2022

- Baixar: https://visualstudio.microsoft.com/
- Instalar com componentes: C++ Desktop Development, CMake

### 2. Instalar Dependencies via vcpkg

```powershell
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg install glfw3 ffmpeg openssl libpng --triplet x64-windows
```

### 3. Compilar

```powershell
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=..\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build . --config Release
```

## Opção 3: Usar WSL2 + MXE (Se estiver no Windows)

Se você está no Windows e tem WSL2:

```bash
# No WSL2
git clone https://github.com/mxe/mxe.git
cd mxe
make MXE_TARGETS=x86_64-w64-mingw32.shared glfw3 ffmpeg openssl libpng
```

Depois usar o MXE para compilar o projeto.

## Troubleshooting

### Erro: Biblioteca não encontrada

- Verifique se as bibliotecas estão no PATH ou CMAKE_FIND_ROOT_PATH
- Use `pkg-config` se disponível
- Ou especifique manualmente: `-DGLFW_ROOT=/caminho/para/glfw`

### Erro: DLL não encontrada

- Copie as DLLs necessárias para o mesmo diretório do executável
- Ou adicione ao PATH do Windows

## Notas

- Compilar no Windows nativo geralmente é mais fácil que cross-compile
- Visual Studio + vcpkg é a forma mais simples no Windows
- MinGW-w64 funciona bem, mas requer bibliotecas pré-compiladas

