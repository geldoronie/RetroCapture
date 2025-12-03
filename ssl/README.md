# Certificados SSL/TLS

Esta pasta contém os certificados SSL/TLS para habilitar HTTPS no servidor.

## Arquivos necessários

- `server.crt` - Certificado SSL/TLS (arquivo .crt ou .pem)
- `server.key` - Chave privada do certificado

## Gerar certificado auto-assinado (desenvolvimento)

```bash
# Gerar chave privada
openssl genrsa -out server.key 2048

# Gerar certificado auto-assinado com Subject Alternative Names (SAN)
# Isso permite usar o certificado com localhost, 127.0.0.1 e outros hostnames
openssl req -new -x509 -key server.key -out server.crt -days 365 \
  -subj "/C=BR/ST=State/L=City/O=RetroCapture/CN=localhost" \
  -addext "subjectAltName=DNS:localhost,DNS:*.localhost,IP:127.0.0.1,IP:::1"
```

**Alternativa mais simples (sem SAN):**
```bash
# Gerar certificado auto-assinado (válido por 365 dias)
openssl req -new -x509 -key server.key -out server.crt -days 365
```

Durante a geração, você será perguntado sobre:
- Country Name
- State/Province
- City
- Organization
- Common Name (importante: use o hostname ou IP do servidor, ex: localhost)

**Nota**: Certificados auto-assinados gerarão avisos de segurança no navegador. 
Para desenvolvimento, você precisará aceitar o certificado manualmente no navegador.

## Let's Encrypt (produção)

Para produção, use certificados do Let's Encrypt:

```bash
sudo certbot certonly --standalone -d seu-dominio.com
```

Os certificados estarão em `/etc/letsencrypt/live/seu-dominio.com/`

## Segurança

⚠️ **IMPORTANTE**: Nunca commite a chave privada (`server.key`) no repositório!

Adicione ao `.gitignore`:
```
ssl/server.key
ssl/*.key
```

## Uso

Os certificados serão copiados automaticamente para o diretório de build durante a compilação.

