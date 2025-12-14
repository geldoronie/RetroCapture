# Resumo: Compilação Windows a partir do Linux

## 🎯 Alternativas Disponíveis

### 1. **MinGW-w64** ⭐ (Recomendado)
- ✅ Mais simples e direto
- ✅ Nativo do Linux
- ✅ Suporta CMake nativamente
- ⚠️ Algumas bibliotecas precisam ser compiladas separadamente

**Instalação:**
```bash
# Arch/Manjaro
sudo pacman -S mingw-w64-gcc

# Ubuntu/Debian
sudo apt-get install mingw-w64 g++-mingw-w64
```

**Uso:**
```bash
./build-windows.sh
```

### 2. **MXE (M cross environment)**
- ✅ Facilita compilação de dependências
- ✅ Scripts automatizados
- ⚠️ Requer compilar todas as dependências (demora)

### 3. **GitHub Actions / CI/CD**
- ✅ Compilação automática
- ✅ Ambiente Windows real
- ⚠️ Requer acesso à internet
- ⚠️ Não permite debug local

### 4. **Docker com Windows**
- ✅ Ambiente isolado
- ✅ Pode usar Visual Studio
- ⚠️ Imagens grandes
- ⚠️ Mais lento

### 5. **Wine + Visual Studio**
- ✅ Ferramentas nativas Windows
- ⚠️ Pode ser instável
- ⚠️ Complexo de configurar

## 📋 Arquivos Criados

1. **`cmake/toolchain-mingw-w64.cmake`** - Toolchain file para CMake
2. **`build-windows.sh`** - Script automatizado de build (instala dependências)
3. **`setup-mxe.sh`** - Script para configurar MXE automaticamente
4. **`docs/WINDOWS_CROSS_COMPILE.md`** - Documentação completa
5. **`docs/MXE_SETUP.md`** - Guia de configuração do MXE

## 🚀 Como Usar

### Opção A: Com MXE (Recomendado - mais fácil)

**Passo 1:** Configurar MXE (compila todas as dependências)
```bash
./setup-mxe.sh
```
⏱️ Tempo: 30-60 minutos (primeira vez)

**Passo 2:** Compilar RetroCapture
```bash
./build-windows.sh
```
O script detectará automaticamente o MXE e o usará.

**Passo 3:** Executável gerado
```
build-windows/bin/retrocapture.exe
```

### Opção B: Sem MXE (MinGW-w64 direto)

**Passo 1:** Instalar MinGW-w64
```bash
# Arch/Manjaro
sudo pacman -S mingw-w64-gcc

# Ubuntu/Debian  
sudo apt-get install mingw-w64 g++-mingw-w64
```

**Passo 2:** Compilar (script instala dependências básicas)
```bash
./build-windows.sh
```

**Nota:** Bibliotecas como GLFW, FFmpeg precisarão ser compiladas manualmente ou via MXE.

## ⚠️ Limitações

1. **Dependências**: FFmpeg, OpenSSL, GLFW precisam estar disponíveis para MinGW-w64
   - Opção: Usar MXE para compilar dependências
   - Opção: Usar bibliotecas pré-compiladas (se disponíveis)

2. **Media Foundation**: Pode ter limitações no MinGW-w64
   - Testar em Windows real é essencial
   - Pode precisar usar DirectShow como fallback

3. **Testes**: Sempre testar em ambiente Windows real

## 🔄 Próximos Passos

1. ✅ Toolchain file criado
2. ✅ Script de build criado
3. ⏳ Testar compilação com MinGW-w64
4. ⏳ Resolver dependências (FFmpeg, OpenSSL, GLFW)
5. ⏳ Configurar GitHub Actions para builds automáticos
6. ⏳ Testar executável em Windows real

## 📝 Notas

- O executável gerado será estático (ou parcialmente estático)
- Algumas DLLs podem ser necessárias no Windows
- Testar em diferentes versões do Windows é recomendado

