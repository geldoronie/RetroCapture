# Como Gerar o Instalador Windows

## Pré-requisitos

- Docker e Docker Compose instalados
- Acesso à internet (para baixar dependências)

## Gerando o Instalador

Execute o script de build do instalador:

```bash
./build-windows-installer.sh
```

Este script irá:

1. **Compilar a aplicação** usando Docker (se necessário)
2. **Gerar o instalador** usando CPack com NSIS
3. **Criar o arquivo** `RetroCapture-{VERSION}-Windows-Setup.exe` no diretório raiz

## O que o Instalador Inclui

O instalador inclui automaticamente:

- ✅ `retrocapture.exe` (executável principal)
- ✅ DLLs dependentes (OpenGL, GLFW, FFmpeg, OpenSSL, etc.)
- ✅ Shaders (`shaders/shaders_glsl/`)
- ✅ Assets (`assets/`)
- ✅ Web Portal (`src/web/`)
- ✅ Certificados SSL (`ssl/`)

## Estrutura de Instalação

Por padrão, o instalador instala em:

```
C:\Program Files\RetroCapture\
├── bin\
│   ├── retrocapture.exe
│   └── *.dll (dependências)
├── share\
│   └── retrocapture\
│       ├── shaders\
│       ├── assets\
│       ├── web\
│       └── ssl\
```

## Funcionalidades do Instalador

- ✅ Instalação em diretório personalizável
- ✅ Atalho no Menu Iniciar
- ✅ Desinstalador automático
- ✅ Desinstala versão anterior automaticamente (se existir)
- ✅ Opção de adicionar ao PATH (configurável)

## Gerando Instalador Manualmente

Se preferir gerar o instalador manualmente:

```bash
# 1. Compilar a aplicação
./docker-build-windows.sh

# 2. Entrar no diretório de build
cd build-windows

# 3. Gerar instalador
cpack -G NSIS
```

## Alternativa: Gerar MSI (WiX)

Para gerar um instalador MSI (Windows Installer) em vez de NSIS:

1. Instale WiX Toolset no sistema
2. Modifique `CMakeLists.txt`:
   ```cmake
   set(CPACK_GENERATOR "WIX")
   ```
3. Execute `cpack -G WIX`

## Troubleshooting

### NSIS não encontrado

Se você receber um erro sobre NSIS não estar disponível:

1. **No Linux**: Instale NSIS localmente:

   ```bash
   # Ubuntu/Debian
   sudo apt-get install nsis

   # Arch/Manjaro
   sudo pacman -S nsis
   ```

2. **No Docker**: O NSIS já está incluído no `Dockerfile.windows`

### Instalador não gerado

Verifique:

- Se o build foi bem-sucedido (`build-windows/bin/retrocapture.exe` existe)
- Se CPack está configurado no `CMakeLists.txt`
- Se NSIS está instalado (localmente ou no container)

### DLLs faltando no instalador

O CPack deve copiar automaticamente as DLLs. Se algumas estiverem faltando:

1. Verifique se as DLLs estão no diretório `build-windows/bin/`
2. Adicione manualmente ao `CMakeLists.txt` se necessário:
   ```cmake
   install(FILES "${CMAKE_BINARY_DIR}/bin/missing.dll"
       DESTINATION bin
       COMPONENT application
   )
   ```

## Personalização

Para personalizar o instalador, edite a seção CPack no `CMakeLists.txt`:

- **Nome do instalador**: `CPACK_PACKAGE_NAME`
- **Versão**: `CPACK_PACKAGE_VERSION` (usa `PROJECT_VERSION`)
- **Ícone**: `CPACK_PACKAGE_ICON`
- **Licença**: `CPACK_RESOURCE_FILE_LICENSE`
- **Atalhos**: `CPACK_NSIS_CREATE_ICONS_EXTRA`

## Referências

- [CPack Documentation](https://cmake.org/cmake/help/latest/manual/cpack.1.html)
- [CPack NSIS Generator](https://cmake.org/cmake/help/latest/cpack_gen/nsis.html)
- [NSIS Documentation](https://nsis.sourceforge.io/Docs/)
