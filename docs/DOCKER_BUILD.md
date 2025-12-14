# Compilação para Windows usando Docker

Esta é a **forma mais simples e confiável** de compilar o RetroCapture para Windows a partir do Linux.

## 🎯 Por que usar Docker?

- ✅ **Ambiente isolado** - Não interfere com seu sistema
- ✅ **Reproduzível** - Mesmo ambiente em qualquer máquina
- ✅ **Sem conflitos** - Dependências isoladas
- ✅ **Fácil de limpar** - Basta remover o container
- ✅ **Cache persistente** - MXE é cacheado entre builds

## 📋 Pré-requisitos

- Docker instalado
- Docker Compose instalado
- ~15GB de espaço em disco (para MXE e dependências)

### Instalar Docker (se necessário)

**Arch/Manjaro:**
```bash
sudo pacman -S docker docker-compose
sudo systemctl enable --now docker
sudo usermod -aG docker $USER
# Faça logout e login novamente
```

**Ubuntu/Debian:**
```bash
sudo apt-get install docker.io docker-compose
sudo systemctl enable --now docker
sudo usermod -aG docker $USER
# Faça logout e login novamente
```

## 🚀 Uso Rápido

### Opção 1: Script Helper (Mais Simples) ⭐

```bash
# Primeira vez (compila MXE e dependências - 30-60 min)
./build-windows-docker.sh

# Builds seguintes (muito mais rápido)
./build-windows-docker.sh
```

### Opção 2: Docker Compose Direto

**Primeira vez (compila MXE e dependências)**

```bash
# Build da imagem Docker (pode demorar 30-60 minutos na primeira vez)
docker-compose build build-windows

# Compilar o RetroCapture
docker-compose run --rm build-windows
```

**Builds subsequentes (muito mais rápido)**

```bash
# Apenas compilar (MXE já está cacheado)
docker-compose run --rm build-windows
```

## 📁 Onde estão os arquivos compilados?

Os arquivos compilados estarão em:
```
./build-windows/bin/retrocapture.exe
```

E também todas as DLLs necessárias estarão no mesmo diretório.

## 🔧 Opções Avançadas

### Rebuild completo (limpar cache do MXE)

```bash
# Remover volume de cache
docker-compose down -v

# Rebuild completo
docker-compose build --no-cache build-windows
docker-compose run --rm build-windows
```

### Acessar o container interativamente

```bash
# Entrar no container
docker-compose run --rm --entrypoint /bin/bash build-windows

# Dentro do container, você pode:
cd /workspace
mkdir -p build-windows && cd build-windows
cmake .. -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
cmake --build . -j$(nproc)
```

### Usar menos/mais jobs paralelos

Edite o `Dockerfile.windows` e altere `-j$(nproc)` para `-j4` (ou outro número).

### Build de debug

Modifique o script `docker-build-windows.sh` ou entre no container e execute:

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
cmake --build . -j$(nproc)
```

## 🐛 Troubleshooting

### Erro: "Missing requirement: 7za, autopoint, bison, flex, gdk-pixbuf-csource"

**Problema:** O MXE está reclamando de dependências faltantes durante o bootstrap.

**Solução:** O Dockerfile já foi atualizado para incluir todas as dependências necessárias. Se ainda ocorrer:

1. Rebuild a imagem sem cache:
```bash
docker-compose build --no-cache build-windows
```

2. Verifique se todas as dependências estão instaladas no container:
```bash
docker-compose run --rm --entrypoint /bin/bash build-windows
# Dentro do container:
which 7za autopoint bison flex gdk-pixbuf-csource
```

### Erro: "Cannot connect to the Docker daemon"

```bash
# Verificar se o Docker está rodando
sudo systemctl status docker

# Iniciar o Docker
sudo systemctl start docker

# Adicionar seu usuário ao grupo docker (se necessário)
sudo usermod -aG docker $USER
# Faça logout e login novamente
```

### Erro: "Permission denied"

```bash
# Adicionar usuário ao grupo docker
sudo usermod -aG docker $USER
# Faça logout e login novamente
```

### Build do MXE falha

O Dockerfile já tenta lidar com erros comuns (como `lame` e `llvm`). Se ainda falhar:

1. Verifique os logs:
```bash
docker-compose build build-windows 2>&1 | tee build.log
```

2. Entre no container e tente manualmente:
```bash
docker-compose run --rm --entrypoint /bin/bash build-windows
cd /opt/mxe
make glfw3 MXE_TARGETS=x86_64-w64-mingw32.shared
```

### Container sem espaço em disco

```bash
# Limpar containers e imagens não usadas
docker system prune -a

# Limpar volumes não usados (cuidado: remove cache do MXE!)
docker volume prune
```

### Rebuild apenas do RetroCapture (não do MXE)

Se apenas o código do RetroCapture mudou, você pode rebuildar apenas o código:

```bash
# Remover apenas o build anterior
rm -rf build-windows

# Recompilar (MXE já está cacheado)
docker-compose run --rm build-windows
```

## 📊 Comparação com outras abordagens

| Método | Complexidade | Tempo Setup | Reproduzibilidade | Isolamento |
|--------|--------------|-------------|-------------------|------------|
| **Docker** | ⭐⭐ Baixa | ⭐⭐⭐ Rápido | ⭐⭐⭐ Excelente | ⭐⭐⭐ Total |
| MXE Local | ⭐⭐⭐ Média | ⭐⭐ Médio | ⭐⭐ Boa | ⭐ Nenhum |
| MinGW-w64 | ⭐⭐⭐⭐ Alta | ⭐⭐⭐ Rápido | ⭐ Baixa | ⭐ Nenhum |

## 💡 Dicas

1. **Primeira build é lenta** - MXE precisa compilar muitas dependências (30-60 min)
2. **Builds seguintes são rápidas** - Cache do Docker acelera muito
3. **Use volumes** - O cache do MXE é mantido entre builds
4. **Limpe quando necessário** - `docker system prune` libera espaço

## 🔗 Ver também

- [MXE Setup (local)](MXE_SETUP.md) - Para setup sem Docker
- [Windows Cross Compile](WINDOWS_CROSS_COMPILE.md) - Alternativas gerais

