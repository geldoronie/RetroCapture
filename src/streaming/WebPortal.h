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
     * Define o subtítulo da página web
     * @param subtitle Subtítulo da página
     */
    void setSubtitle(const std::string &subtitle) { m_subtitle = subtitle; }

    /**
     * Define textos editáveis do portal
     */
    void setTexts(
        const std::string &streamInfo, const std::string &quickActions, const std::string &compatibility,
        const std::string &status, const std::string &codec, const std::string &resolution,
        const std::string &streamUrl, const std::string &copyUrl, const std::string &openNewTab,
        const std::string &supported, const std::string &format, const std::string &codecInfo,
        const std::string &supportedBrowsers, const std::string &formatInfo, const std::string &codecInfoValue,
        const std::string &connecting);

    /**
     * Define parâmetros de desempenho do HLS
     */
    void setHLSParameters(
        bool lowLatencyMode,
        float backBufferLength,
        float maxBufferLength,
        float maxMaxBufferLength,
        bool enableWorker);

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
        const float primaryLight[4], const float primaryDark[4],
        const float secondary[4], const float secondaryHighlight[4],
        const float cardHeader[4], const float border[4],
        const float success[4], const float warning[4], const float danger[4], const float info[4]);

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

    /**
     * Substitui texto no HTML de forma segura
     * @param html HTML onde substituir (modificado in-place)
     * @param oldText Texto antigo a ser substituído
     * @param newText Novo texto
     */
    void replaceTextInHTML(std::string &html, const std::string &oldText, const std::string &newText) const;

    HTTPServer *m_httpServer = nullptr;
    std::string m_title = "RetroCapture Stream";                 // Título da página web
    std::string m_subtitle = "Streaming de vídeo em tempo real"; // Subtítulo
    std::string m_imagePath = "logo.png";                        // Caminho da imagem para o título (padrão: logo.png)
    std::string m_backgroundImagePath;                           // Caminho da imagem de fundo (opcional)

    // Parâmetros HLS
    // HLS Performance parameters - alinhados com valores padrão da UI
    bool m_hlsLowLatencyMode = false;      // Desabilitado por padrão: permite mais prefetch e buffer maior
    float m_hlsBackBufferLength = 40.0f;   // 40s = 20 segmentos de 2s (alinhado com HLS_SEGMENT_COUNT)
    float m_hlsMaxBufferLength = 30.0f;    // 30s = 15 segmentos de 2s (buffer maior para fluidez)
    float m_hlsMaxMaxBufferLength = 60.0f; // 60s = 30 segmentos de 2s (buffer máximo generoso)
    bool m_hlsEnableWorker = false;        // Desabilitado por padrão: mais compatível com Chrome

    // Textos editáveis
    std::string m_textStreamInfo = "Informações do Stream";
    std::string m_textQuickActions = "Ações Rápidas";
    std::string m_textCompatibility = "Compatibilidade";
    std::string m_textStatus = "Status";
    std::string m_textCodec = "Codec";
    std::string m_textResolution = "Resolução";
    std::string m_textStreamUrl = "URL do Stream";
    std::string m_textCopyUrl = "Copiar URL";
    std::string m_textOpenNewTab = "Abrir em Nova Aba";
    std::string m_textSupported = "Suportado";
    std::string m_textFormat = "Formato";
    std::string m_textCodecInfo = "Codec";
    std::string m_textSupportedBrowsers = "Chrome, Firefox, Safari, Edge";
    std::string m_textFormatInfo = "HLS (HTTP Live Streaming)";
    std::string m_textCodecInfoValue = "H.264/AAC";
    std::string m_textConnecting = "Conectando...";

    // Cores do portal baseadas no styleguide RetroCapture (RGBA, valores 0.0-1.0)
    // Primary - Retro Teal #0A7A83
    float m_colorPrimary[4] = {0.039f, 0.478f, 0.514f, 1.0f};
    // Primary Light - Mint Screen Glow #6FC4C0
    float m_colorPrimaryLight[4] = {0.435f, 0.769f, 0.753f, 1.0f};
    // Primary Dark - Deep Retro #0F3E42
    float m_colorPrimaryDark[4] = {0.059f, 0.243f, 0.259f, 1.0f};
    // Secondary - Cyan Oscilloscope #47B3CE
    float m_colorSecondary[4] = {0.278f, 0.702f, 0.808f, 1.0f};
    // Secondary Highlight - Phosphor Glow #C9F2E7
    float m_colorSecondaryHighlight[4] = {0.788f, 0.949f, 0.906f, 1.0f};
    // Dark Background #1D1F21
    float m_colorBackground[4] = {0.114f, 0.122f, 0.129f, 1.0f};
    // Text Light #F8F8F2
    float m_colorText[4] = {0.973f, 0.973f, 0.949f, 1.0f};
    // Card Header (usa Primary Dark)
    float m_colorCardHeader[4] = {0.059f, 0.243f, 0.259f, 1.0f};
    // Border (usa Primary com transparência)
    float m_colorBorder[4] = {0.039f, 0.478f, 0.514f, 0.5f};
    // Success #45D6A4
    float m_colorSuccess[4] = {0.271f, 0.839f, 0.643f, 1.0f};
    // Warning #F3C93E
    float m_colorWarning[4] = {0.953f, 0.788f, 0.243f, 1.0f};
    // Error #D9534F
    float m_colorDanger[4] = {0.851f, 0.325f, 0.310f, 1.0f};
    // Info #4CBCE6
    float m_colorInfo[4] = {0.298f, 0.737f, 0.902f, 1.0f};
};
