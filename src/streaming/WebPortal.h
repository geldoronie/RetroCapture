#pragma once

#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <vector>

/**
 * WebPortal - Responsável por servir o portal web do RetroCapture
 * 
 * Esta classe gerencia todas as funcionalidades relacionadas ao portal web:
 * - Servir página HTML principal
 * - Servir arquivos estáticos (CSS, JS)
 * - Gerenciar rotas HTTP relacionadas ao portal
 * - Encontrar e ler arquivos do diretório web
 */
// Forward declaration
class HTTPServer;

class WebPortal
{
public:
    WebPortal();
    ~WebPortal() = default;
    
    /**
     * Define a referência ao HTTPServer para enviar dados via SSL quando necessário
     * @param httpServer Referência ao HTTPServer
     */
    void setHTTPServer(HTTPServer* httpServer) { m_httpServer = httpServer; }

    /**
     * Processa uma requisição HTTP e determina se é uma requisição do portal web
     * @param request Requisição HTTP recebida
     * @return true se é uma requisição do portal web, false caso contrário
     */
    bool isWebPortalRequest(const std::string& request) const;

    /**
     * Serve a página HTML principal
     * @param clientFd File descriptor do socket do cliente
     * @param basePrefix Prefixo base para URLs (ex: "/retrocapture")
     * @return true se servido com sucesso, false caso contrário
     */
    bool serveWebPage(int clientFd, const std::string& basePrefix = "") const;

    /**
     * Serve um arquivo estático (CSS, JS, etc.)
     * @param clientFd File descriptor do socket do cliente
     * @param filePath Caminho do arquivo relativo ao diretório web
     * @return true se servido com sucesso, false caso contrário
     */
    bool serveStaticFile(int clientFd, const std::string& filePath) const;

    /**
     * Envia resposta 404 Not Found
     * @param clientFd File descriptor do socket do cliente
     */
    void send404(int clientFd) const;

    /**
     * Processa uma requisição HTTP do portal web
     * @param clientFd File descriptor do socket do cliente
     * @param request Requisição HTTP recebida
     * @return true se a requisição foi processada, false caso contrário
     */
    bool handleRequest(int clientFd, const std::string& request) const;
    
    /**
     * Extrai o prefixo base da requisição (para suporte a proxy reverso)
     * @param request Requisição HTTP
     * @return Prefixo base (ex: "/retrocapture") ou vazio se não houver
     */
    std::string extractBasePrefix(const std::string& request) const;
    
    /**
     * Substitui URLs relativas no HTML com o prefixo base
     * @param html Conteúdo HTML
     * @param basePrefix Prefixo base (ex: "/retrocapture")
     * @return HTML com URLs atualizadas
     */
    std::string injectBasePrefix(const std::string& html, const std::string& basePrefix) const;

private:
    /**
     * Encontra o diretório web em várias localizações possíveis
     * @return Caminho absoluto do diretório web
     */
    std::string getWebDirectory() const;

    /**
     * Lê o conteúdo de um arquivo
     * @param filePath Caminho completo do arquivo
     * @return Conteúdo do arquivo como string, vazio se erro
     */
    std::string readFileContent(const std::string& filePath) const;

    /**
     * Obtém o tipo MIME baseado na extensão do arquivo
     * @param filePath Caminho do arquivo
     * @return Tipo MIME (ex: "text/html", "text/css", "application/javascript")
     */
    std::string getContentType(const std::string& filePath) const;

    /**
     * Extrai o caminho do arquivo da requisição HTTP
     * @param request Requisição HTTP
     * @return Caminho do arquivo (ex: "/index.html", "/style.css")
     */
    std::string extractFilePath(const std::string& request) const;
    
    /**
     * Envia dados através do HTTPServer (suporta SSL se necessário)
     * @param clientFd File descriptor do cliente
     * @param data Dados para enviar
     * @param size Tamanho dos dados
     * @return Número de bytes enviados, ou -1 se erro
     */
    ssize_t sendData(int clientFd, const void* data, size_t size) const;

    HTTPServer* m_httpServer = nullptr;
};

