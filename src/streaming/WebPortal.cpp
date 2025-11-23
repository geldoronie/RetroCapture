#include "WebPortal.h"
#include "HTTPServer.h"
#include "../utils/Logger.h"
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>

WebPortal::WebPortal()
{
    // Verificar se o diretório web existe
    std::string webDir = getWebDirectory();
    if (webDir.empty() || !std::filesystem::exists(webDir))
    {
        LOG_ERROR("Web directory not found. Web portal may not work correctly.");
    }
    else
    {
        LOG_INFO("Web portal initialized. Web directory: " + webDir);
    }
}

bool WebPortal::isWebPortalRequest(const std::string& request) const
{
    // NÃO capturar requisições de stream (deixar para HTTPTSStreamer processar)
    if (request.find("/stream") != std::string::npos ||
        request.find("/segment_") != std::string::npos ||
        request.find(".m3u8") != std::string::npos ||
        request.find(".ts") != std::string::npos)
    {
        return false;
    }
    
    // Verificar se é uma requisição para o portal web
    // Aceita tanto "/" quanto "/retrocapture/" ou outros prefixos
    if (request.find("GET / ") != std::string::npos ||
        request.find("GET / HTTP/") != std::string::npos ||
        request.find("GET /index.html") != std::string::npos ||
        request.find("GET /style.css") != std::string::npos ||
        request.find("GET /player.js") != std::string::npos ||
        request.find("GET /favicon.ico") != std::string::npos ||
        request.find("/index.html") != std::string::npos ||
        request.find("/style.css") != std::string::npos ||
        request.find("/player.js") != std::string::npos)
    {
        return true;
    }
    return false;
}

bool WebPortal::handleRequest(int clientFd, const std::string& request) const
{
    // Ignorar favicon.ico
    if (request.find("GET /favicon.ico") != std::string::npos ||
        request.find("/favicon.ico") != std::string::npos)
    {
        send404(clientFd);
        return true;
    }

    // Extrair prefixo base (para suporte a proxy reverso)
    std::string basePrefix = extractBasePrefix(request);

    // PRIMEIRO: Verificar se é arquivo estático (antes de verificar página principal)
    // Isso é importante porque arquivos estáticos têm prioridade
    std::string filePath = extractFilePath(request);
    if (!filePath.empty())
    {
        LOG_INFO("Serving static file: " + filePath + " (basePrefix: " + basePrefix + ")");
        bool result = serveStaticFile(clientFd, filePath);
        if (result)
        {
            return true;
        }
        // Se falhou, continuar para verificar se é página principal
    }

    // Verificar se é página web principal
    if (request.find("GET / ") != std::string::npos ||
        request.find("GET / HTTP/") != std::string::npos ||
        request.find("GET /index.html") != std::string::npos ||
        request.find("/index.html") != std::string::npos)
    {
        return serveWebPage(clientFd, basePrefix);
    }
    
    LOG_WARN("No file path extracted from request: " + request.substr(0, std::min(100UL, request.length())));

    // Requisição não reconhecida
    return false;
}

bool WebPortal::serveWebPage(int clientFd, const std::string& basePrefix) const
{
    std::string webDir = getWebDirectory();
    std::string indexPath = webDir + "/index.html";

    std::string html = readFileContent(indexPath);
    if (html.empty())
    {
        LOG_ERROR("Failed to read index.html, using fallback");
        // Fallback simples se não conseguir ler o arquivo
        std::string streamUrl = basePrefix.empty() ? "/stream" : basePrefix + "/stream";
        html = "<!DOCTYPE html><html><head><title>RetroCapture Stream</title></head><body><h1>RetroCapture Stream</h1><p>Erro ao carregar página. Stream disponível em: <a href=\"" + streamUrl + "\">" + streamUrl + "</a></p></body></html>";
    }
    else if (!basePrefix.empty())
    {
        // Injetar prefixo base nos links relativos
        html = injectBasePrefix(html, basePrefix);
        LOG_INFO("Injected base prefix: " + basePrefix);
    }

    LOG_INFO("Serving web page (HTML size: " + std::to_string(html.length()) + " bytes)");

    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: text/html; charset=utf-8\r\n";
    response << "Content-Length: " << html.length() << "\r\n";
    response << "Connection: close\r\n";
    response << "Cache-Control: no-cache\r\n";
    response << "\r\n";
    response << html;

    std::string responseStr = response.str();
    ssize_t sent = sendData(clientFd, responseStr.c_str(), responseStr.length());
    if (sent < 0)
    {
        LOG_ERROR("Failed to send web page to client");
        return false;
    }
    else
    {
        LOG_INFO("Web page sent successfully (" + std::to_string(sent) + " bytes)");
        return true;
    }
}

bool WebPortal::serveStaticFile(int clientFd, const std::string& filePath) const
{
    std::string webDir = getWebDirectory();
    std::string fullPath = webDir + "/" + filePath;
    
    LOG_INFO("Attempting to serve static file - filePath: " + filePath + ", fullPath: " + fullPath);

    std::string content = readFileContent(fullPath);
    if (content.empty())
    {
        LOG_ERROR("Failed to read static file: " + fullPath);
        LOG_ERROR("Web directory: " + webDir);
        LOG_ERROR("Requested file path: " + filePath);
        send404(clientFd);
        return false;
    }

    std::string contentType = getContentType(filePath);

    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: " << contentType << "; charset=utf-8\r\n";
    response << "Content-Length: " << content.length() << "\r\n";
    response << "Connection: close\r\n";
    response << "Cache-Control: public, max-age=3600\r\n";
    response << "\r\n";
    response << content;

    std::string responseStr = response.str();
    ssize_t sent = sendData(clientFd, responseStr.c_str(), responseStr.length());
    if (sent < 0)
    {
        LOG_ERROR("Failed to send static file to client");
        return false;
    }
    else
    {
        LOG_INFO("Static file sent successfully (" + std::to_string(sent) + " bytes): " + filePath);
        return true;
    }
}

void WebPortal::send404(int clientFd) const
{
    const char *response = "HTTP/1.1 404 Not Found\r\n"
                           "Content-Type: text/plain\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "404 Not Found";
    sendData(clientFd, response, strlen(response));
}

ssize_t WebPortal::sendData(int clientFd, const void* data, size_t size) const
{
    if (m_httpServer)
    {
        return m_httpServer->sendData(clientFd, data, size);
    }
    // Fallback para send() direto se HTTPServer não estiver configurado
    return send(clientFd, data, size, MSG_NOSIGNAL);
}

std::string WebPortal::getWebDirectory() const
{
    // Tentar encontrar o diretório web em várias localizações possíveis
    std::vector<std::string> possiblePaths = {
        "./web",         // Diretório atual (build/bin/web)
        "../web",        // Um nível acima
        "../../web",     // Dois níveis acima
        "src/web",       // Do diretório raiz do projeto
        "../src/web",    // Um nível acima + src/web
        "../../src/web", // Dois níveis acima + src/web
    };

    for (const auto &path : possiblePaths)
    {
        std::filesystem::path webPath(path);
        if (std::filesystem::exists(webPath) && std::filesystem::is_directory(webPath))
        {
            return std::filesystem::absolute(webPath).string();
        }
    }

    // Fallback: tentar usar caminho relativo ao executável
    return "./web";
}

std::string WebPortal::readFileContent(const std::string& filePath) const
{
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open())
    {
        LOG_ERROR("Failed to open file: " + filePath);
        return "";
    }

    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

std::string WebPortal::getContentType(const std::string& filePath) const
{
    // Determinar tipo MIME baseado na extensão
    // Usar find ao invés de ends_with para compatibilidade com C++17
    size_t len = filePath.length();
    
    if (len >= 5 && filePath.substr(len - 5) == ".html")
    {
        return "text/html";
    }
    else if (len >= 4 && filePath.substr(len - 4) == ".htm")
    {
        return "text/html";
    }
    else if (len >= 4 && filePath.substr(len - 4) == ".css")
    {
        return "text/css";
    }
    else if (len >= 3 && filePath.substr(len - 3) == ".js")
    {
        return "application/javascript";
    }
    else if (len >= 4 && filePath.substr(len - 4) == ".png")
    {
        return "image/png";
    }
    else if (len >= 4 && filePath.substr(len - 4) == ".jpg")
    {
        return "image/jpeg";
    }
    else if (len >= 5 && filePath.substr(len - 5) == ".jpeg")
    {
        return "image/jpeg";
    }
    else if (len >= 4 && filePath.substr(len - 4) == ".svg")
    {
        return "image/svg+xml";
    }
    else if (len >= 4 && filePath.substr(len - 4) == ".ico")
    {
        return "image/x-icon";
    }
    else
    {
        return "application/octet-stream";
    }
}

std::string WebPortal::extractFilePath(const std::string& request) const
{
    // Extrair caminho do arquivo da requisição HTTP
    // Exemplo: "GET /style.css HTTP/1.1" -> "style.css"
    // Exemplo: "GET /retrocapture/style.css HTTP/1.1" -> "style.css" (remove prefixo)
    
    size_t getPos = request.find("GET /");
    if (getPos == std::string::npos)
    {
        return "";
    }

    size_t startPos = getPos + 5; // Após "GET /"
    size_t endPos = request.find(" ", startPos);
    if (endPos == std::string::npos)
    {
        endPos = request.find(" HTTP/", startPos);
        if (endPos == std::string::npos)
        {
            endPos = request.find("\r\n", startPos);
            if (endPos == std::string::npos)
            {
                return "";
            }
        }
    }

    std::string path = request.substr(startPos, endPos - startPos);
    
    // Remover query string se existir
    size_t queryPos = path.find('?');
    if (queryPos != std::string::npos)
    {
        path = path.substr(0, queryPos);
    }
    
    // Verificar se é um arquivo estático conhecido
    bool isStaticFile = (path.find("style.css") != std::string::npos ||
                         path.find("player.js") != std::string::npos ||
                         path.find(".css") != std::string::npos ||
                         path.find(".js") != std::string::npos);
    
    if (!isStaticFile)
    {
        return "";
    }
    
    // Remover barra inicial se existir
    if (!path.empty() && path[0] == '/')
    {
        path = path.substr(1);
    }
    
    // Remover prefixo comum se existir (ex: "retrocapture/style.css" -> "style.css")
    // Isso é necessário porque o nginx pode estar enviando o path completo
    std::vector<std::string> commonPrefixes = {"retrocapture/", "retrocapture"};
    for (const auto& prefix : commonPrefixes)
    {
        if (path.find(prefix) == 0)
        {
            path = path.substr(prefix.length());
            // Remover barra inicial se ainda existir
            if (!path.empty() && path[0] == '/')
            {
                path = path.substr(1);
            }
            break;
        }
    }
    
    // Extrair apenas o nome do arquivo (sem diretórios extras)
    size_t lastSlash = path.find_last_of('/');
    if (lastSlash != std::string::npos && lastSlash < path.length() - 1)
    {
        path = path.substr(lastSlash + 1);
    }

    return path;
}

std::string WebPortal::extractBasePrefix(const std::string& request) const
{
    // Tentar extrair prefixo base de várias formas:
    
    // 1. Do header X-Forwarded-Prefix (padrão para proxy reverso)
    size_t prefixHeaderPos = request.find("X-Forwarded-Prefix:");
    if (prefixHeaderPos != std::string::npos)
    {
        size_t valueStart = request.find(":", prefixHeaderPos) + 1;
        while (valueStart < request.length() && (request[valueStart] == ' ' || request[valueStart] == '\t'))
        {
            valueStart++;
        }
        size_t valueEnd = request.find("\r\n", valueStart);
        if (valueEnd == std::string::npos)
        {
            valueEnd = request.find("\n", valueStart);
        }
        if (valueEnd != std::string::npos)
        {
            std::string prefix = request.substr(valueStart, valueEnd - valueStart);
            // Remover espaços em branco
            while (!prefix.empty() && (prefix.back() == ' ' || prefix.back() == '\t' || prefix.back() == '\r'))
            {
                prefix.pop_back();
            }
            if (!prefix.empty())
            {
                // Garantir que começa com / mas não termina com /
                if (prefix[0] != '/')
                {
                    prefix = "/" + prefix;
                }
                if (prefix.length() > 1 && prefix.back() == '/')
                {
                    prefix.pop_back();
                }
                return prefix;
            }
        }
    }
    
    // 2. Do path da requisição (se contiver um prefixo conhecido)
    size_t getPos = request.find("GET /");
    if (getPos != std::string::npos)
    {
        size_t startPos = getPos + 5; // Após "GET /"
        size_t endPos = request.find(" ", startPos);
        if (endPos == std::string::npos)
        {
            endPos = request.find("\r\n", startPos);
        }
        if (endPos != std::string::npos)
        {
            std::string path = request.substr(startPos, endPos - startPos);
            // Verificar se começa com um prefixo conhecido
            if (path.find("retrocapture/") == 0)
            {
                return "/retrocapture";
            }
        }
    }
    
    return ""; // Sem prefixo
}

std::string WebPortal::injectBasePrefix(const std::string& html, const std::string& basePrefix) const
{
    if (basePrefix.empty())
    {
        return html;
    }
    
    std::string result = html;
    
    // Substituir URLs relativas que começam com /
    // Padrões a substituir:
    // href="/style.css" -> href="/retrocapture/style.css"
    // src="/player.js" -> src="/retrocapture/player.js"
    // href="/stream" -> href="/retrocapture/stream"
    
    std::vector<std::pair<std::string, std::string>> replacements = {
        {"href=\"/style.css\"", "href=\"" + basePrefix + "/style.css\""},
        {"href='/style.css'", "href='" + basePrefix + "/style.css'"},
        {"src=\"/player.js\"", "src=\"" + basePrefix + "/player.js\""},
        {"src='/player.js'", "src='" + basePrefix + "/player.js'"},
        {"href=\"/stream\"", "href=\"" + basePrefix + "/stream\""},
        {"href='/stream'", "href='" + basePrefix + "/stream'"},
        {"href=\"/stream.m3u8\"", "href=\"" + basePrefix + "/stream.m3u8\""},
        {"href='/stream.m3u8'", "href='" + basePrefix + "/stream.m3u8'"},
    };
    
    for (const auto& [oldStr, newStr] : replacements)
    {
        size_t pos = 0;
        while ((pos = result.find(oldStr, pos)) != std::string::npos)
        {
            result.replace(pos, oldStr.length(), newStr);
            pos += newStr.length();
        }
    }
    
    // Também substituir URLs que começam com / mas não são recursos externos
    // Usar regex seria melhor, mas vamos fazer substituições simples
    // Substituir padrões como: "/stream" (mas não "https://" ou "http://")
    
    return result;
}

