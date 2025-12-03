# Planejamento: Página Web com Player de Streaming

## 🎯 Objetivo

Adicionar uma página web simples que permita visualizar o stream MPEG-TS diretamente no navegador, sem necessidade de usar players externos (VLC, ffplay, etc.).

## 📋 Análise da Situação Atual

### Servidor HTTP Atual
- **Localização**: `HTTPTSStreamer::handleClient()` em `src/streaming/HTTPTSStreamer.cpp`
- **Comportamento**: 
  - Aceita apenas requisições `GET /stream`
  - Retorna stream MPEG-TS com `Content-Type: video/mp2t`
  - Qualquer outra requisição recebe `404 Not Found`
- **Arquitetura**: Thread separada por cliente (`handleClient` é chamado em thread detached)

### Limitações do HTML5 Video Tag
- **MPEG-TS**: Não é suportado nativamente pelo `<video>` tag em navegadores
- **HLS**: Seria necessário converter para HLS (m3u8 + segmentos), o que adiciona complexidade
- **Alternativas**: Usar bibliotecas JavaScript que suportam MPEG-TS

## 🎨 Opções de Implementação

### Opção 1: HTML5 Video Tag com MIME Type (Mais Simples)
**Prós:**
- Implementação muito simples
- Sem dependências externas
- Funciona em alguns navegadores (Chrome/Edge com codecs apropriados)

**Contras:**
- Suporte limitado (não funciona em Firefox, Safari)
- Pode não funcionar mesmo no Chrome dependendo dos codecs

**Implementação:**
```html
<video controls autoplay>
  <source src="/stream" type="video/mp2t">
  Seu navegador não suporta MPEG-TS.
</video>
```

### Opção 2: HLS.js (Recomendado para Compatibilidade)
**Prós:**
- Funciona em todos os navegadores modernos
- Suporte nativo a HLS (HTTP Live Streaming)
- Biblioteca madura e bem mantida

**Contras:**
- Requer conversão de MPEG-TS para HLS (m3u8 + segmentos)
- Adiciona complexidade ao servidor
- Requer segmentação do stream

**Implementação:**
- Converter stream MPEG-TS para HLS usando FFmpeg
- Servir playlist `.m3u8` e segmentos `.ts`
- Usar HLS.js no cliente

### Opção 3: Video.js com Plugin MPEG-TS (Melhor para MPEG-TS Direto)
**Prós:**
- Suporta MPEG-TS diretamente via plugins
- Player profissional e customizável
- Boa experiência do usuário

**Contras:**
- Requer biblioteca externa (Video.js)
- Pode precisar de plugins adicionais
- Mais complexo que opção 1

### Opção 4: MSE (Media Source Extensions) com MPEG-TS Parser
**Prós:**
- Controle total sobre o player
- Suporte a MPEG-TS direto
- Sem necessidade de conversão

**Contras:**
- Implementação complexa
- Requer parser MPEG-TS em JavaScript
- Pode ter problemas de sincronização

## ✅ Recomendação: Abordagem Híbrida (Fase 1 + Fase 2)

### Fase 1: Implementação Simples (MVP)
**Objetivo**: Funcionar rapidamente com mínimo de código

1. **HTML5 Video Tag** com fallback
2. **Página HTML simples** servida pelo mesmo servidor
3. **Rota `/` ou `/index.html`** para servir a página
4. **Rota `/stream`** continua servindo MPEG-TS

**Vantagens:**
- Implementação rápida
- Funciona em Chrome/Edge (maioria dos usuários)
- Base para melhorias futuras

### Fase 2: Melhorias (Futuro)
1. Adicionar HLS.js para compatibilidade universal
2. Converter MPEG-TS para HLS no servidor
3. Adicionar controles customizados
4. Adicionar informações do stream (codec, bitrate, etc.)

## 🏗️ Arquitetura Proposta

### Estrutura de Arquivos
```
src/streaming/
├── HTTPTSStreamer.h/cpp (modificado)
└── web/
    ├── index.html (página principal)
    ├── player.js (JavaScript opcional para controles)
    └── style.css (CSS opcional para estilização)
```

### Modificações Necessárias

#### 1. HTTPTSStreamer::handleClient()
```cpp
void HTTPTSStreamer::handleClient(int clientFd)
{
    // ... código existente ...
    
    std::string request(buffer);
    
    // Detectar tipo de requisição
    if (request.find("GET / ") != std::string::npos || 
        request.find("GET /index.html") != std::string::npos)
    {
        // Servir página HTML
        serveWebPage(clientFd);
        close(clientFd);
        return;
    }
    else if (request.find("GET /stream") != std::string::npos)
    {
        // Servir stream MPEG-TS (código existente)
        // ... código atual ...
    }
    else
    {
        // 404 para outras requisições
        send404(clientFd);
        close(clientFd);
        return;
    }
}
```

#### 2. Nova Função: serveWebPage()
```cpp
void HTTPTSStreamer::serveWebPage(int clientFd)
{
    // Ler HTML do arquivo ou usar string inline
    std::string html = getWebPageHTML();
    
    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: text/html; charset=utf-8\r\n";
    response << "Content-Length: " << html.length() << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << html;
    
    std::string responseStr = response.str();
    send(clientFd, responseStr.c_str(), responseStr.length(), MSG_NOSIGNAL);
}
```

#### 3. Nova Função: getWebPageHTML()
```cpp
std::string HTTPTSStreamer::getWebPageHTML()
{
    // Opção 1: HTML inline (mais simples)
    return R"(
<!DOCTYPE html>
<html>
<head>
    <title>RetroCapture Stream</title>
    <style>
        body { 
            margin: 0; 
            padding: 20px; 
            background: #1a1a1a; 
            color: #fff; 
            font-family: Arial, sans-serif;
        }
        .container {
            max-width: 1200px;
            margin: 0 auto;
        }
        video {
            width: 100%;
            max-width: 1280px;
            background: #000;
        }
        .info {
            margin-top: 20px;
            padding: 15px;
            background: #2a2a2a;
            border-radius: 5px;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>RetroCapture Stream</h1>
        <video controls autoplay muted>
            <source src="/stream" type="video/mp2t">
            Seu navegador não suporta MPEG-TS diretamente.
            <br>Use um player externo: <a href="/stream">Clique aqui para baixar o stream</a>
        </video>
        <div class="info">
            <p><strong>Stream URL:</strong> <code>http://localhost:8080/stream</code></p>
            <p><strong>Status:</strong> <span id="status">Conectando...</span></p>
            <p><em>Nota: MPEG-TS pode não funcionar em todos os navegadores. 
            Se o vídeo não aparecer, use VLC ou ffplay com a URL acima.</em></p>
        </div>
    </div>
    <script>
        const video = document.querySelector('video');
        const status = document.getElementById('status');
        
        video.addEventListener('loadstart', () => {
            status.textContent = 'Carregando...';
        });
        
        video.addEventListener('canplay', () => {
            status.textContent = 'Reproduzindo';
        });
        
        video.addEventListener('error', (e) => {
            status.textContent = 'Erro: ' + (video.error ? video.error.message : 'Formato não suportado');
        });
    </script>
</body>
</html>
    )";
    
    // Opção 2: Ler de arquivo (mais flexível)
    // return readFile("src/streaming/web/index.html");
}
```

## 📝 Plano de Implementação

### Etapa 1: Estrutura Básica
1. ✅ Criar função `serveWebPage()` em `HTTPTSStreamer`
2. ✅ Criar função `getWebPageHTML()` com HTML inline
3. ✅ Modificar `handleClient()` para detectar `GET /` e `GET /index.html`
4. ✅ Adicionar função helper `send404()` para consistência

### Etapa 2: HTML Básico
1. ✅ Criar HTML simples com `<video>` tag
2. ✅ Adicionar CSS básico para estilização
3. ✅ Adicionar JavaScript para feedback de status
4. ✅ Adicionar informações do stream (URL, status)

### Etapa 3: Melhorias (Opcional)
1. ⬜ Adicionar controles customizados
2. ⬜ Adicionar informações de codec/bitrate
3. ⬜ Adicionar contador de clientes conectados
4. ⬜ Melhorar design com tema escuro

### Etapa 4: Testes
1. ✅ Testar em Chrome/Edge
2. ✅ Testar em Firefox (deve mostrar mensagem de fallback)
3. ✅ Testar em Safari (deve mostrar mensagem de fallback)
4. ✅ Verificar que `/stream` continua funcionando

## 🎨 Design da Página

### Layout Proposto
```
┌─────────────────────────────────────┐
│  RetroCapture Stream                │
├─────────────────────────────────────┤
│                                     │
│  [Video Player - Full Width]        │
│                                     │
├─────────────────────────────────────┤
│  Stream URL: http://localhost:8080  │
│  Status: Reproduzindo               │
│                                     │
│  Nota: MPEG-TS pode não funcionar  │
│  em todos os navegadores...         │
└─────────────────────────────────────┘
```

### Características
- **Tema escuro**: Compatível com ambiente de streaming
- **Responsivo**: Funciona em diferentes tamanhos de tela
- **Informações claras**: URL do stream e status visíveis
- **Fallback amigável**: Mensagem clara se não funcionar

## 🔧 Considerações Técnicas

### Performance
- HTML inline é servido uma vez por requisição (muito leve)
- Não adiciona overhead significativo ao servidor
- Página é estática, sem processamento adicional

### Segurança
- Apenas serve HTML estático
- Não processa dados do cliente
- Mesmas considerações de segurança do stream atual

### Compatibilidade
- **Chrome/Edge**: Pode funcionar com MPEG-TS (depende dos codecs instalados)
- **Firefox**: Não suporta MPEG-TS nativamente
- **Safari**: Não suporta MPEG-TS nativamente
- **Fallback**: Link direto para `/stream` para uso em players externos

## 🚀 Próximos Passos

1. **Implementar Fase 1** (HTML básico com video tag)
2. **Testar em diferentes navegadores**
3. **Coletar feedback dos usuários**
4. **Decidir se implementa Fase 2** (HLS.js para compatibilidade universal)

## 📚 Referências

- [HTML5 Video Element](https://developer.mozilla.org/en-US/docs/Web/HTML/Element/video)
- [HLS.js Documentation](https://github.com/video-dev/hls.js/)
- [Video.js Documentation](https://videojs.com/)
- [Media Source Extensions](https://developer.mozilla.org/en-US/docs/Web/API/Media_Source_Extensions_API)

## 💡 Ideias Futuras

- **WebSocket para status**: Atualizar status do stream em tempo real
- **Controles customizados**: Play/pause, volume, fullscreen customizados
- **Informações do stream**: Mostrar codec, bitrate, resolução, FPS
- **Múltiplos streams**: Se houver múltiplos streamers ativos
- **Gravador de stream**: Botão para gravar stream localmente
- **Screenshot**: Capturar frame atual do stream

