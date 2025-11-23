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
    void setHTTPServer(HTTPServer *httpServer) { m_httpServer = httpServer; }

    /**
     * Define o título da página web
     * @param title Título da página
     */
    void setTitle(const std::string &title) { m_title = title; }

    /**
     * Define o caminho da imagem para o título (opcional)
     * @param imagePath Caminho da imagem (vazio para usar ícone padrão)
     */
    void setImagePath(const std::string &imagePath) { m_imagePath = imagePath; }

    /**
     * Define o caminho da imagem de fundo (opcional)
     * @param imagePath Caminho da imagem de fundo (vazio para usar cor sólida)
     */
    void setBackgroundImagePath(const std::string &imagePath) { m_backgroundImagePath = imagePath; }

    /**
     * Define as cores do portal
     * @param bg Cor de fundo (RGBA, 0.0-1.0)
     * @param text Cor do texto (RGBA, 0.0-1.0)
     * @param primary Cor primária (RGBA, 0.0-1.0)
     * @param secondary Cor secundária/cards (RGBA, 0.0-1.0)
     * @param cardHeader Cor do cabeçalho dos cards (RGBA, 0.0-1.0)
     * @param border Cor das bordas (RGBA, 0.0-1.0)
     * @param success Cor de sucesso (RGBA, 0.0-1.0)
     * @param warning Cor de aviso (RGBA, 0.0-1.0)
     * @param danger Cor de erro (RGBA, 0.0-1.0)
     */
    void setColors(
        const float bg[4], const float text[4], const float primary[4],
        const float secondary[4], const float cardHeader[4], const float border[4],
        const float success[4], const float warning[4], const float danger[4]);

    /**
     * Processa uma requisição HTTP e determina se é uma requisição do portal web
     * @param request Requisição HTTP recebida
     * @return true se é uma requisição do portal web, false caso contrário
     */
    bool isWebPortalRequest(const std::string &request) const;

    /**
     * Serve a página HTML principal
     * @param clientFd File descriptor do socket do cliente
     * @param basePrefix Prefixo base para URLs (ex: "/retrocapture")
     * @return true se servido com sucesso, false caso contrário
     */
    bool serveWebPage(int clientFd, const std::string &basePrefix = "") const;

    /**
     * Serve um arquivo estático (CSS, JS, etc.)
     * @param clientFd File descriptor do socket do cliente
     * @param filePath Caminho do arquivo relativo ao diretório web
     * @return true se servido com sucesso, false caso contrário
     */
    bool serveStaticFile(int clientFd, const std::string &filePath) const;

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
    bool handleRequest(int clientFd, const std::string &request) const;

    /**
     * Extrai o prefixo base da requisição (para suporte a proxy reverso)
     * @param request Requisição HTTP
     * @return Prefixo base (ex: "/retrocapture") ou vazio se não houver
     */
    std::string extractBasePrefix(const std::string &request) const;

    /**
     * Substitui URLs relativas no HTML com o prefixo base
     * @param html Conteúdo HTML
     * @param basePrefix Prefixo base (ex: "/retrocapture")
     * @return HTML com URLs atualizadas
     */
    std::string injectBasePrefix(const std::string &html, const std::string &basePrefix) const;

    /**
     * Gera CSS customizado com as cores e imagem de fundo configuradas
     * @param basePrefix Prefixo base para URLs (ex: "/retrocapture")
     * @return CSS customizado como string
     */
    std::string generateCustomCSS(const std::string &basePrefix = "") const;

private:
    /**
     * Encontra o diretório web em várias localizações possíveis
     * @return Caminho absoluto do diretório web
     */
    std::string getWebDirectory() const;

    /**
     * Encontra arquivo de imagem em vários locais possíveis (assets/)
     * Prioridade: 1) Caminho absoluto, 2) ~/.config/retrocapture/assets/, 3) ./assets/ (diretório do executável)
     * @param relativePath Caminho relativo ou nome do arquivo
     * @return Caminho absoluto do arquivo encontrado, ou string vazia se não encontrado
     */
    std::string findAssetFile(const std::string &relativePath) const;

    /**
     * Lê o conteúdo de um arquivo
     * @param filePath Caminho completo do arquivo
     * @return Conteúdo do arquivo como string, vazio se erro
     */
    std::string readFileContent(const std::string &filePath) const;

    /**
     * Obtém o tipo MIME baseado na extensão do arquivo
     * @param filePath Caminho do arquivo
     * @return Tipo MIME (ex: "text/html", "text/css", "application/javascript")
     */
    std::string getContentType(const std::string &filePath) const;

    /**
     * Extrai o caminho do arquivo da requisição HTTP
     * @param request Requisição HTTP
     * @return Caminho do arquivo (ex: "/index.html", "/style.css")
     */
    std::string extractFilePath(const std::string &request) const;

    /**
     * Envia dados através do HTTPServer (suporta SSL se necessário)
     * @param clientFd File descriptor do cliente
     * @param data Dados para enviar
     * @param size Tamanho dos dados
     * @return Número de bytes enviados, ou -1 se erro
     */
    ssize_t sendData(int clientFd, const void *data, size_t size) const;

    HTTPServer *m_httpServer = nullptr;
    std::string m_title = "RetroCapture Stream"; // Título da página web
    std::string m_imagePath;                     // Caminho da imagem para o título (opcional)
    std::string m_backgroundImagePath;           // Caminho da imagem de fundo (opcional)

    // Cores do portal (RGBA, valores 0.0-1.0)
    float m_colorBackground[4] = {0.102f, 0.102f, 0.102f, 1.0f};
    float m_colorText[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float m_colorPrimary[4] = {0.290f, 0.620f, 1.0f, 1.0f};
    float m_colorSecondary[4] = {0.165f, 0.165f, 0.165f, 1.0f};
    float m_colorCardHeader[4] = {0.102f, 0.102f, 0.102f, 1.0f};
    float m_colorBorder[4] = {0.4f, 0.4f, 0.4f, 1.0f};
    float m_colorSuccess[4] = {0.298f, 0.686f, 0.314f, 1.0f};
    float m_colorWarning[4] = {1.0f, 0.596f, 0.0f, 1.0f};
    float m_colorDanger[4] = {0.957f, 0.263f, 0.212f, 1.0f};
};
