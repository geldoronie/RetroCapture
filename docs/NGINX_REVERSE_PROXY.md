# Configuração de Proxy Reverso com Nginx

Este documento descreve como configurar o RetroCapture para funcionar atrás de um proxy reverso nginx.

## Configuração do Nginx

Adicione a seguinte configuração ao seu arquivo de configuração do nginx:

```nginx
location /retrocapture/ {
    proxy_pass http://127.0.0.1:8080/;
    
    # Headers essenciais para proxy reverso
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto $scheme;
    
    # Header importante: informa ao backend qual é o prefixo base
    proxy_set_header X-Forwarded-Prefix /retrocapture;
    
    # Headers opcionais
    add_header Cache-Control "public, max-age=31536000";
    add_header Access-Control-Allow-Origin "*";
    
    # Para streaming, desabilitar buffering
    proxy_buffering off;
    proxy_cache off;
    
    # Timeouts para streaming
    proxy_read_timeout 300s;
    proxy_connect_timeout 75s;
}
```

## Como Funciona

1. **X-Forwarded-Prefix**: Este header informa ao RetroCapture qual é o prefixo base da URL. O RetroCapture usa este header para ajustar automaticamente todos os links no HTML.

2. **proxy_pass**: O nginx remove o prefixo `/retrocapture/` e envia apenas o path relativo para o backend (ex: `/retrocapture/index.html` → `/index.html`).

3. **Ajuste Automático**: O RetroCapture detecta o prefixo através do header `X-Forwarded-Prefix` e automaticamente:
   - Substitui links no HTML (ex: `/style.css` → `/retrocapture/style.css`)
   - Ajusta URLs no JavaScript para usar o prefixo correto

## Exemplo de Requisições

- Cliente acessa: `https://realmify.app/retrocapture/index.html`
- Nginx envia para backend: `http://127.0.0.1:8080/index.html` (com header `X-Forwarded-Prefix: /retrocapture`)
- RetroCapture detecta o prefixo e ajusta os links no HTML
- Cliente recebe HTML com links corretos: `/retrocapture/style.css`, `/retrocapture/player.js`, etc.

## Testando

1. Configure o nginx com a configuração acima
2. Reinicie o nginx: `sudo systemctl reload nginx`
3. Acesse: `https://realmify.app/retrocapture/`
4. Verifique no console do navegador se os recursos estão sendo carregados corretamente

## Troubleshooting

### Recursos retornando 404

- Verifique se o header `X-Forwarded-Prefix` está sendo enviado
- Verifique os logs do nginx: `sudo tail -f /var/log/nginx/error.log`
- Verifique os logs do RetroCapture para ver se o prefixo está sendo detectado

### Links ainda apontando para raiz

- Certifique-se de que o header `X-Forwarded-Prefix` está configurado corretamente
- Verifique se o RetroCapture está detectando o prefixo (veja logs)

### Problemas com streaming

- Certifique-se de que `proxy_buffering` está desabilitado
- Aumente os timeouts se necessário
- Verifique se o streaming está acessível em `https://realmify.app/retrocapture/stream.m3u8`

