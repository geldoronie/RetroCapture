# Windows Installer - Análise de Alternativas

## Contexto do Projeto

O RetroCapture já possui:

- ✅ Build via Docker para Windows (MinGW/MXE)
- ✅ AppImage para Linux (com shaders, assets, web portal, SSL)
- ✅ CMake como sistema de build
- ✅ Estrutura bem definida de dependências

## Alternativas Disponíveis

### 1. **CPack (CMake Package)** ⭐ **RECOMENDADO**

**Vantagens:**

- ✅ **Integração nativa com CMake** - já usamos CMake
- ✅ Suporta múltiplos backends (NSIS, WiX, ZIP, 7Z)
- ✅ Configuração via `CMakeLists.txt` (sem arquivos extras)
- ✅ Funciona no mesmo processo de build
- ✅ Suporta instalação de componentes (executável, shaders, assets, etc.)
- ✅ Pode gerar NSIS ou WiX automaticamente
- ✅ Cross-platform (mesmo script funciona para Linux/Windows)

**Desvantagens:**

- ⚠️ Menos flexível que NSIS/Inno Setup para casos muito específicos
- ⚠️ Requer conhecimento de CMake (já temos)

**Complexidade:** Baixa (integração direta)
**Tamanho do instalador:** Médio (depende do backend escolhido)

---

### 2. **Inno Setup**

**Vantagens:**

- ✅ **Muito popular** (70%+ dos desenvolvedores)
- ✅ **Sintaxe simples** e fácil de aprender
- ✅ **GUI disponível** (Inno Setup Compiler)
- ✅ Gera executável único (.exe) compactado
- ✅ Suporta desinstalador automático
- ✅ Suporta atualizações e patches
- ✅ Muitos exemplos e documentação

**Desvantagens:**

- ⚠️ Arquivo de script separado (.iss)
- ⚠️ Precisa ser executado após o build
- ⚠️ Não integrado ao CMake (mas pode ser chamado via script)

**Complexidade:** Média
**Tamanho do instalador:** Pequeno (compactação eficiente)

---

### 3. **NSIS (Nullsoft Scriptable Install System)**

**Vantagens:**

- ✅ **Muito flexível** e poderoso
- ✅ **Muitos plugins** disponíveis
- ✅ Gera executável único (.exe)
- ✅ Suporta instalação silenciosa
- ✅ Usado por muitos projetos open-source

**Desvantagens:**

- ⚠️ **Sintaxe mais complexa** que Inno Setup
- ⚠️ Sem GUI (apenas scripts)
- ⚠️ Curva de aprendizado maior
- ⚠️ Arquivo de script separado (.nsi)

**Complexidade:** Alta
**Tamanho do instalador:** Pequeno a médio

---

### 4. **WiX Toolset**

**Vantagens:**

- ✅ Gera **MSI packages** (padrão enterprise)
- ✅ Integração com Windows Installer
- ✅ Suporta atualizações automáticas via Windows Update
- ✅ Ideal para distribuição corporativa

**Desvantagens:**

- ⚠️ **Muito complexo** (XML-based, curva de aprendizado alta)
- ⚠️ Sem GUI nativo (ferramentas de terceiros disponíveis)
- ⚠️ Overkill para aplicações desktop simples
- ⚠️ Requer conhecimento de Windows Installer

**Complexidade:** Muito Alta
**Tamanho do instalador:** Médio a grande

---

## Recomendação: **CPack com Backend NSIS**

### Por quê?

1. **Integração perfeita**: Já usamos CMake, então CPack se integra naturalmente
2. **Menos arquivos**: Tudo configurado no `CMakeLists.txt`
3. **Automação**: Pode ser executado no mesmo script de build Docker
4. **Flexibilidade**: Pode mudar de backend (NSIS → WiX) facilmente
5. **Manutenção**: Um único lugar para configurar (CMakeLists.txt)

### Estrutura Proposta

```
CMakeLists.txt
├── Configuração CPack
│   ├── Nome do instalador
│   ├── Versão
│   ├── Componentes (executável, shaders, assets, web, ssl)
│   ├── Diretórios de instalação
│   └── Ícone/logo
└── Backend: NSIS (ou WiX se preferir MSI)
```

### Arquivos a Incluir no Instalador

Baseado no `build-appimage.sh`:

- ✅ `retrocapture.exe` (executável)
- ✅ DLLs dependentes (OpenGL, GLFW, FFmpeg, etc.)
- ✅ `shaders/shaders_glsl/` (diretório completo)
- ✅ `assets/` (logo, etc.)
- ✅ `src/web/` (web portal)
- ✅ `ssl/` (certificados SSL)
- ✅ Ícone/logo para o instalador
- ✅ Desinstalador

### Exemplo de Configuração CPack

```cmake
# No final do CMakeLists.txt
include(CPack)

set(CPACK_GENERATOR "NSIS")  # ou "WIX" para MSI
set(CPACK_PACKAGE_NAME "RetroCapture")
set(CPACK_PACKAGE_VENDOR "RetroCapture")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Video capture with RetroArch shader support")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "RetroCapture")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}/assets/logo.png")

# Componentes
set(CPACK_COMPONENT_APPLICATION_DISPLAY_NAME "RetroCapture Application")
set(CPACK_COMPONENT_SHADERS_DISPLAY_NAME "Shader Presets")
set(CPACK_COMPONENT_ASSETS_DISPLAY_NAME "Assets")
set(CPACK_COMPONENT_WEB_DISPLAY_NAME "Web Portal")
set(CPACK_COMPONENT_SSL_DISPLAY_NAME "SSL Certificates")

# Instalar arquivos
install(TARGETS retrocapture
    RUNTIME DESTINATION bin
    COMPONENT application
)

install(DIRECTORY shaders/shaders_glsl
    DESTINATION share/retrocapture/shaders
    COMPONENT shaders
)

install(DIRECTORY assets
    DESTINATION share/retrocapture
    COMPONENT assets
)

install(DIRECTORY src/web
    DESTINATION share/retrocapture
    COMPONENT web
)

install(DIRECTORY ssl
    DESTINATION share/retrocapture
    COMPONENT ssl
)
```

### Script de Build

```bash
#!/bin/bash
# build-windows-installer.sh

# 1. Build via Docker (já existe)
./docker-build-windows.sh

# 2. Gerar instalador via CPack
cd build-windows
cpack -G NSIS
# ou
cpack -G WIX  # para MSI
```

---

## Alternativa: Inno Setup (se preferir mais controle)

Se você preferir mais controle visual e uma sintaxe mais simples, **Inno Setup** é uma excelente segunda opção:

### Vantagens sobre CPack:

- ✅ GUI para criar scripts
- ✅ Mais fácil de customizar visualmente
- ✅ Melhor para ajustes finos de UI do instalador

### Desvantagens:

- ⚠️ Arquivo `.iss` separado para manter
- ⚠️ Precisa ser executado após o build

### Exemplo de Script Inno Setup (.iss)

```pascal
[Setup]
AppName=RetroCapture
AppVersion=0.3.0
DefaultDirName={pf}\RetroCapture
DefaultGroupName=RetroCapture
OutputDir=installer
OutputBaseFilename=RetroCapture-Setup-0.3.0
Compression=lzma2
SolidCompression=yes
SetupIconFile=assets\logo.ico

[Files]
Source: "build-windows\bin\retrocapture.exe"; DestDir: "{app}\bin"
Source: "build-windows\bin\*.dll"; DestDir: "{app}\bin"
Source: "shaders\shaders_glsl\*"; DestDir: "{app}\shaders\shaders_glsl"; Flags: recursesubdirs
Source: "assets\*"; DestDir: "{app}\assets"; Flags: recursesubdirs
Source: "src\web\*"; DestDir: "{app}\web"; Flags: recursesubdirs
Source: "ssl\*"; DestDir: "{app}\ssl"; Flags: recursesubdirs

[Icons]
Name: "{group}\RetroCapture"; Filename: "{app}\bin\retrocapture.exe"
Name: "{commondesktop}\RetroCapture"; Filename: "{app}\bin\retrocapture.exe"

[Run]
Filename: "{app}\bin\retrocapture.exe"; Description: "Launch RetroCapture"; Flags: nowait postinstall skipifsilent
```

---

## Comparação Rápida

| Critério              | CPack (NSIS) | Inno Setup         | NSIS Manual        | WiX          |
| --------------------- | ------------ | ------------------ | ------------------ | ------------ |
| **Integração CMake**  | ✅ Nativa    | ❌ Script separado | ❌ Script separado | ✅ Via CPack |
| **Facilidade**        | ⭐⭐⭐⭐     | ⭐⭐⭐⭐⭐         | ⭐⭐⭐             | ⭐⭐         |
| **Flexibilidade**     | ⭐⭐⭐⭐     | ⭐⭐⭐⭐           | ⭐⭐⭐⭐⭐         | ⭐⭐⭐⭐⭐   |
| **Tamanho**           | Médio        | Pequeno            | Pequeno            | Médio        |
| **Manutenção**        | ⭐⭐⭐⭐⭐   | ⭐⭐⭐⭐           | ⭐⭐⭐             | ⭐⭐⭐       |
| **Curva Aprendizado** | Baixa        | Baixa              | Média              | Alta         |

---

## Decisão Final

### 🏆 **Recomendação: CPack com NSIS**

**Motivos:**

1. Integração perfeita com o build existente
2. Menos arquivos para manter
3. Pode ser automatizado no Docker build
4. Flexível o suficiente para nossas necessidades
5. Fácil de mudar para WiX (MSI) no futuro se necessário

### 📋 Próximos Passos

1. Adicionar configuração CPack no `CMakeLists.txt`
2. Criar script `build-windows-installer.sh`
3. Testar geração do instalador no Docker
4. Documentar processo de release

---

## Referências

- [CPack Documentation](https://cmake.org/cmake/help/latest/manual/cpack.1.html)
- [NSIS Documentation](https://nsis.sourceforge.io/Docs/)
- [Inno Setup Documentation](https://jrsoftware.org/ishelp/)
- [WiX Toolset](https://wixtoolset.org/)
