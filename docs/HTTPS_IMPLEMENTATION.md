# Implementação de HTTPS no RetroCapture

## Visão Geral

Foi implementado suporte a HTTPS usando OpenSSL, permitindo que o servidor HTTP funcione tanto em modo HTTP quanto HTTPS, dependendo da configuração.

## Arquitetura

### Classe HTTPServer

A classe `HTTPServer` foi criada para abstrair a criação e gerenciamento de sockets HTTP/HTTPS:

- **Localização**: `src/streaming/HTTPServer.h` e `src/streaming/HTTPServer.cpp`
- **Responsabilidades**:
  - Criar e gerenciar sockets do servidor
  - Aceitar conexões de clientes
  - Enviar/receber dados (com suporte SSL se habilitado)
  - Gerenciar certificados SSL/TLS
  - Fechar conexões adequadamente

### Compilação Condicional

O suporte a HTTPS é **opcional** e compilado apenas se `ENABLE_HTTPS` estiver definido:

```cmake
option(ENABLE_HTTPS "Enable HTTPS support with OpenSSL" OFF)
```

Se `ENABLE_HTTPS` estiver desabilitado, o código funciona normalmente em HTTP, mas as funções SSL retornam erro ou são ignoradas.

## Dependências

### OpenSSL

Para compilar com suporte HTTPS, é necessário ter OpenSSL instalado:

```bash
# Ubuntu/Debian
sudo apt-get install libssl-dev

# Fedora/CentOS
sudo dnf install openssl-devel

# Arch Linux
sudo pacman -S openssl
```

## Configuração no CMakeLists.txt

Adicione ao `CMakeLists.txt`:

```cmake
# Opção para habilitar HTTPS
option(ENABLE_HTTPS "Enable HTTPS support with OpenSSL" OFF)

if(ENABLE_HTTPS)
    find_package(OpenSSL REQUIRED)
    if(OpenSSL_FOUND)
        target_link_libraries(retrocapture PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(retrocapture PRIVATE ENABLE_HTTPS)
        message(STATUS "HTTPS support enabled")
    else()
        message(WARNING "OpenSSL not found, HTTPS support disabled")
        set(ENABLE_HTTPS OFF)
    endif()
endif()
```

## Compilação

### Sem HTTPS (padrão)
```bash
cmake -B build
cmake --build build
```

### Com HTTPS
```bash
cmake -B build -DENABLE_HTTPS=ON
cmake --build build
```

## Uso

### 1. Criar Certificado SSL/TLS

#### Opção A: Certificado Auto-assinado (para desenvolvimento/testes)

```bash
# Gerar chave privada
openssl genrsa -out server.key 2048

# Gerar certificado auto-assinado (válido por 365 dias)
openssl req -new -x509 -key server.key -out server.crt -days 365

# Durante a geração, você será perguntado sobre:
# - Country Name
# - State/Province
# - City
# - Organization
# - Common Name (importante: use o hostname ou IP do servidor)
```

#### Opção B: Let's Encrypt (para produção)

```bash
# Instalar certbot
sudo apt-get install certbot

# Obter certificado
sudo certbot certonly --standalone -d seu-dominio.com

# Os certificados estarão em:
# /etc/letsencrypt/live/seu-dominio.com/fullchain.pem
# /etc/letsencrypt/live/seu-dominio.com/privkey.pem
```

### 2. Configurar no Código

No `HTTPTSStreamer`, antes de chamar `start()`:

```cpp
// Criar instância do HTTPServer
HTTPServer httpServer;

// Configurar certificado SSL (opcional - apenas para HTTPS)
if (enableHTTPS)
{
    if (!httpServer.setSSLCertificate("server.crt", "server.key"))
    {
        LOG_ERROR("Failed to configure SSL certificate");
        return false;
    }
}

// Criar servidor
if (!httpServer.createServer(port))
{
    LOG_ERROR("Failed to create server");
    return false;
}
```

### 3. Usar no HTTPTSStreamer

Substituir chamadas diretas a `socket()`, `accept()`, `send()`, `recv()` por métodos do `HTTPServer`:

```cpp
// Antes:
int clientFd = accept(m_serverSocket, ...);
send(clientFd, data, size, MSG_NOSIGNAL);
recv(clientFd, buffer, size, 0);

// Depois:
int clientFd = httpServer.acceptClient();
httpServer.sendData(clientFd, data, size);
httpServer.receiveData(clientFd, buffer, size);
```

## Integração com HTTPTSStreamer

Para integrar completamente, seria necessário:

1. Adicionar membro `HTTPServer m_httpServer;` em `HTTPTSStreamer`
2. Substituir `m_serverSocket` por uso de `m_httpServer`
3. Adicionar métodos de configuração SSL na interface pública
4. Atualizar `getStreamUrl()` para retornar `https://` quando SSL estiver habilitado

## Segurança

### Recomendações

1. **Produção**: Use certificados de uma CA confiável (Let's Encrypt, etc.)
2. **Desenvolvimento**: Certificados auto-assinados são aceitáveis, mas navegadores mostrarão aviso
3. **Renovação**: Certificados Let's Encrypt expiram a cada 90 dias - configure renovação automática
4. **Chave Privada**: Mantenha a chave privada (`server.key`) segura e com permissões restritas:
   ```bash
   chmod 600 server.key
   ```

### Limitações Atuais

- Suporte apenas para TLS 1.2+ (SSLv2 e SSLv3 desabilitados)
- Não há suporte a múltiplos certificados (SNI)
- Não há verificação de certificados de clientes (mutual TLS)

## Testando

### Teste Local com Certificado Auto-assinado

1. Gere certificado auto-assinado (veja acima)
2. Configure no código
3. Compile com `-DENABLE_HTTPS=ON`
4. Execute o servidor
5. Acesse `https://localhost:8080` no navegador
6. Aceite o aviso de certificado não confiável (normal para auto-assinado)

### Verificar Certificado

```bash
# Verificar certificado
openssl x509 -in server.crt -text -noout

# Testar conexão SSL
openssl s_client -connect localhost:8080 -showcerts
```

## Próximos Passos

1. Integrar `HTTPServer` completamente no `HTTPTSStreamer`
2. Adicionar opção na UI para habilitar/desabilitar HTTPS
3. Adicionar campos para caminho do certificado e chave na UI
4. Implementar suporte a SNI (Server Name Indication)
5. Adicionar verificação de certificados de clientes (mutual TLS) se necessário

