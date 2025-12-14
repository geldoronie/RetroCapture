# Docker Simplificado - Se Quiser Tentar Novamente

Se quiser tentar o Docker novamente no futuro, aqui está uma versão simplificada:

## Problema com a Abordagem Anterior

A abordagem anterior tentou fazer tudo automaticamente, mas tinha problemas:
- Volume Docker sobrescrevendo o MXE compilado
- Bootstrap não sendo executado corretamente
- Dependências faltando na imagem

## Abordagem Simplificada

### Dockerfile Simples

```dockerfile
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive

# Instalar tudo de uma vez
RUN apt-get update && apt-get install -y \
    build-essential cmake git wget pkg-config \
    autotools-dev autoconf automake libtool libtool-bin \
    lzip gperf intltool ruby python3 python3-mako \
    nasm yasm p7zip-full gettext gettext-base \
    bison flex libgdk-pixbuf2.0-dev \
    && rm -rf /var/lib/apt/lists/*

# Clonar e compilar MXE (tudo de uma vez, sem volumes)
WORKDIR /opt
RUN git clone https://github.com/mxe/mxe.git && \
    cd mxe && \
    make -j$(nproc) MXE_TARGETS=x86_64-w64-mingw32.shared \
         glfw3 openssl libpng ffmpeg

WORKDIR /workspace
COPY docker-build-simple.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/docker-build-simple.sh
ENTRYPOINT ["/usr/local/bin/docker-build-simple.sh"]
```

### Script de Build Simples

```bash
#!/bin/bash
cd /workspace
mkdir -p build-windows
cd build-windows
cmake .. -DCMAKE_TOOLCHAIN_FILE=/opt/mxe/usr/x86_64-w64-mingw32.shared/share/cmake/mxe-conf.cmake
cmake --build . -j$(nproc)
```

### docker-compose.yml Simplificado

```yaml
services:
  build:
    build:
      context: .
      dockerfile: Dockerfile.simple
    volumes:
      - .:/workspace:ro
      - ./build-windows:/workspace/build-windows
```

## Diferenças

1. **Sem volume para MXE** - Compila tudo na imagem
2. **Sem scripts complexos** - Tudo direto no Dockerfile
3. **Mais lento para rebuild** - Mas mais confiável

## Uso

```bash
docker-compose -f docker-compose.simple.yml build
docker-compose -f docker-compose.simple.yml run --rm build
```

## Nota

Esta abordagem é mais simples mas tem trade-offs:
- ✅ Mais confiável
- ✅ Menos scripts complexos
- ❌ Rebuild da imagem é mais lento
- ❌ Imagem Docker maior

