#include "WebPortal.h"
#include "HTTPServer.h"
#include "../utils/Logger.h"
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <iomanip>

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

bool WebPortal::isWebPortalRequest(const std::string &request) const
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
        request.find("GET /portal-image") != std::string::npos ||
        request.find("GET /portal-background") != std::string::npos ||
        request.find("/index.html") != std::string::npos ||
        request.find("/style.css") != std::string::npos ||
        request.find("/player.js") != std::string::npos ||
        request.find("/portal-image") != std::string::npos ||
        request.find("/portal-background") != std::string::npos)
    {
        return true;
    }
    return false;
}

bool WebPortal::handleRequest(int clientFd, const std::string &request) const
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

    // Verificar se é requisição para imagem do portal
    if (request.find("GET /portal-image") != std::string::npos ||
        request.find("/portal-image") != std::string::npos)
    {
        if (!m_imagePath.empty())
        {
            // Buscar imagem nos locais padrão (assets/)
            std::string foundImagePath = findAssetFile(m_imagePath);

            if (!foundImagePath.empty() && std::filesystem::exists(foundImagePath))
            {
                // Servir a imagem do portal
                std::string content = readFileContent(foundImagePath);
                if (!content.empty())
                {
                    std::string contentType = getContentType(foundImagePath);
                    std::ostringstream response;
                    response << "HTTP/1.1 200 OK\r\n";
                    response << "Content-Type: " << contentType << "\r\n";
                    response << "Content-Length: " << content.length() << "\r\n";
                    response << "Cache-Control: public, max-age=3600\r\n";
                    response << "Connection: close\r\n";
                    response << "\r\n";
                    response << content;

                    std::string responseStr = response.str();
                    ssize_t sent = sendData(clientFd, responseStr.c_str(), responseStr.length());
                    if (sent >= 0)
                    {
                        LOG_INFO("Portal image served successfully from: " + foundImagePath);
                        return true;
                    }
                }
            }
            else
            {
                LOG_WARN("Portal image not found: " + m_imagePath + " (searched in assets/)");
            }
        }
        send404(clientFd);
        return true;
    }

    // Verificar se é requisição para imagem de fundo do portal
    if (request.find("GET /portal-background") != std::string::npos ||
        request.find("/portal-background") != std::string::npos)
    {
        if (!m_backgroundImagePath.empty())
        {
            // Buscar imagem nos locais padrão (assets/)
            std::string foundBgPath = findAssetFile(m_backgroundImagePath);

            if (!foundBgPath.empty() && std::filesystem::exists(foundBgPath))
            {
                // Servir a imagem de fundo do portal
                std::string content = readFileContent(foundBgPath);
                if (!content.empty())
                {
                    std::string contentType = getContentType(foundBgPath);
                    std::ostringstream response;
                    response << "HTTP/1.1 200 OK\r\n";
                    response << "Content-Type: " << contentType << "\r\n";
                    response << "Content-Length: " << content.length() << "\r\n";
                    response << "Cache-Control: public, max-age=3600\r\n";
                    response << "Connection: close\r\n";
                    response << "\r\n";
                    response << content;

                    std::string responseStr = response.str();
                    ssize_t sent = sendData(clientFd, responseStr.c_str(), responseStr.length());
                    if (sent >= 0)
                    {
                        LOG_INFO("Portal background image served successfully from: " + foundBgPath);
                        return true;
                    }
                }
            }
            else
            {
                LOG_WARN("Portal background image not found: " + m_backgroundImagePath + " (searched in assets/)");
            }
        }
        send404(clientFd);
        return true;
    }

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

bool WebPortal::serveWebPage(int clientFd, const std::string &basePrefix) const
{
    std::string webDir = getWebDirectory();
    std::string indexPath = webDir + "/index.html";

    std::string html = readFileContent(indexPath);
    if (html.empty())
    {
        LOG_ERROR("Failed to read index.html, using fallback");
        // Fallback simples se não conseguir ler o arquivo
        std::string streamUrl = basePrefix.empty() ? "/stream" : basePrefix + "/stream";
        html = "<!DOCTYPE html><html><head><title>" + m_title + "</title></head><body><h1>" + m_title + "</h1><p>Erro ao carregar página. Stream disponível em: <a href=\"" + streamUrl + "\">" + streamUrl + "</a></p></body></html>";
    }
    else
    {
        // Substituir título da página (tag <title>)
        std::string pageTitleTag = "<title>" + m_title + "</title>";
        size_t pageTitlePos = html.find("<title>");
        if (pageTitlePos != std::string::npos)
        {
            size_t pageTitleEnd = html.find("</title>", pageTitlePos);
            if (pageTitleEnd != std::string::npos)
            {
                html.replace(pageTitlePos, pageTitleEnd - pageTitlePos + 8, pageTitleTag);
            }
        }

        // Substituir título no header (novo layout)
        std::string headerTitleTag = "<h1 class=\"portal-title mb-0\">" + m_title + "</h1>";
        size_t headerTitlePos = html.find("<h1 class=\"portal-title mb-0\">");
        if (headerTitlePos == std::string::npos)
        {
            // Fallback para formato antigo
            headerTitlePos = html.find("<h1 class=\"mb-0\">");
            if (headerTitlePos != std::string::npos)
            {
                size_t headerTitleEnd = html.find("</h1>", headerTitlePos);
                if (headerTitleEnd != std::string::npos)
                {
                    html.replace(headerTitlePos, headerTitleEnd - headerTitlePos + 5, headerTitleTag);
                }
            }
        }
        else
        {
            size_t headerTitleEnd = html.find("</h1>", headerTitlePos);
            if (headerTitleEnd != std::string::npos)
            {
                html.replace(headerTitlePos, headerTitleEnd - headerTitlePos + 5, headerTitleTag);
            }
        }

        // Substituir subtítulo (novo layout)
        std::string subtitleTag = "<p class=\"portal-subtitle mb-0\">" + m_subtitle + "</p>";
        size_t subtitlePos = html.find("<p class=\"portal-subtitle mb-0\">");
        if (subtitlePos == std::string::npos)
        {
            // Fallback para formato antigo
            subtitlePos = html.find("<p class=\"text-muted mb-0\">");
            if (subtitlePos != std::string::npos)
            {
                size_t subtitleEnd = html.find("</p>", subtitlePos);
                if (subtitleEnd != std::string::npos)
                {
                    html.replace(subtitlePos, subtitleEnd - subtitlePos + 4, subtitleTag);
                }
            }
        }
        else
        {
            size_t subtitleEnd = html.find("</p>", subtitlePos);
            if (subtitleEnd != std::string::npos)
            {
                html.replace(subtitlePos, subtitleEnd - subtitlePos + 4, subtitleTag);
            }
        }

        // Substituir textos do novo layout moderno
        replaceTextInHTML(html, "RetroCapture Stream", m_title);
        replaceTextInHTML(html, "Streaming de vídeo em tempo real", m_subtitle);
        replaceTextInHTML(html, "Informações do Stream", m_textStreamInfo);
        replaceTextInHTML(html, "Status", m_textStatus);
        replaceTextInHTML(html, "Resolução", m_textResolution);
        replaceTextInHTML(html, "Codec", m_textCodec);
        replaceTextInHTML(html, "URL do Stream", m_textStreamUrl);
        replaceTextInHTML(html, "Formato", m_textFormat);
        replaceTextInHTML(html, "HLS (HTTP Live Streaming)", m_textFormatInfo);
        replaceTextInHTML(html, "Copiar URL", m_textCopyUrl);
        replaceTextInHTML(html, "Abrir em Nova Aba", m_textOpenNewTab);
        replaceTextInHTML(html, "Estatísticas", "Estatísticas");
        replaceTextInHTML(html, "Conectando...", m_textConnecting);

        // Substituir ícone por imagem se especificado (novo layout)
        if (!m_imagePath.empty())
        {
            // Buscar imagem nos locais padrão (assets/)
            std::string foundImagePath = findAssetFile(m_imagePath);

            if (!foundImagePath.empty() && std::filesystem::exists(foundImagePath))
            {
                // Procurar pelo logo-container no novo layout
                std::string logoContainerStart = "<div class=\"logo-container me-3\" id=\"logoContainer\">";
                size_t logoPos = html.find(logoContainerStart);
                if (logoPos != std::string::npos)
                {
                    // Encontrar o fechamento da div logo-container
                    size_t logoEnd = html.find("</div>", logoPos + logoContainerStart.length());
                    if (logoEnd != std::string::npos)
                    {
                        // Criar tag de imagem
                        std::string imgUrl = (basePrefix.empty() ? "/" : basePrefix) + "portal-image";
                        std::string imgTag = "<div class=\"logo-container me-3\" id=\"logoContainer\"><img src=\"" + imgUrl + "\" alt=\"" + m_title + "\" style=\"width: 100%; height: 100%; object-fit: contain; border-radius: 12px;\"></div>";
                        html.replace(logoPos, logoEnd - logoPos + 6, imgTag);
                    }
                }
                else
                {
                    // Fallback para formato antigo
                    std::string iconTag = "<i class=\"bi bi-controller fs-1 text-primary me-3\"></i>";
                    size_t iconPos = html.find(iconTag);
                    if (iconPos != std::string::npos)
                    {
                        std::string imgUrl = (basePrefix.empty() ? "/" : basePrefix) + "portal-image";
                        std::string imgTag = "<img src=\"" + imgUrl + "\" alt=\"" + m_title + "\" class=\"me-3\" style=\"max-height: 48px; width: auto;\">";
                        html.replace(iconPos, iconTag.length(), imgTag);
                    }
                }
            }
        }

        // IMPORTANTE: Remover meta tag upgrade-insecure-requests se HTTPS não estiver ativo
        // Essa tag força o navegador a fazer upgrade de HTTP para HTTPS, causando problemas
        // quando HTTPS está desabilitado e acessando pela rede interna
        // Verificar se HTTPS está realmente ativo através do HTTPServer
        bool httpsActive = false;
        if (m_httpServer)
        {
            httpsActive = m_httpServer->isHTTPS();
        }

        // Se HTTPS não estiver ativo, remover a meta tag upgrade-insecure-requests
        if (!httpsActive)
        {
            // Buscar a meta tag de forma flexível (pode ter espaços diferentes)
            // Padrão: <meta http-equiv="Content-Security-Policy" content="upgrade-insecure-requests">
            size_t metaStart = html.find("<meta");
            while (metaStart != std::string::npos)
            {
                // Encontrar o final da tag meta
                size_t metaEnd = html.find(">", metaStart);
                if (metaEnd == std::string::npos)
                {
                    break;
                }

                // Extrair a tag completa
                std::string metaTag = html.substr(metaStart, metaEnd - metaStart + 1);

                // Verificar se é a tag upgrade-insecure-requests
                if (metaTag.find("Content-Security-Policy") != std::string::npos &&
                    metaTag.find("upgrade-insecure-requests") != std::string::npos)
                {
                    // Remover a tag (incluindo quebra de linha se houver)
                    size_t removeStart = metaStart;
                    size_t removeEnd = metaEnd + 1;

                    // Remover espaços em branco e quebras de linha após a tag
                    while (removeEnd < html.length() &&
                           (html[removeEnd] == ' ' || html[removeEnd] == '\t' || html[removeEnd] == '\n' || html[removeEnd] == '\r'))
                    {
                        removeEnd++;
                    }

                    html.erase(removeStart, removeEnd - removeStart);
                    LOG_INFO("Removida meta tag upgrade-insecure-requests (HTTPS desabilitado)");
                    break; // Tag encontrada e removida, sair do loop
                }

                // Procurar próxima tag meta
                metaStart = html.find("<meta", metaStart + 1);
            }
        }

        // Injetar CSS customizado com cores e imagem de fundo
        std::string customCSS = generateCustomCSS(basePrefix);

        // Inserir CSS customizado antes do fechamento de </head>
        size_t headEndPos = html.find("</head>");
        if (headEndPos != std::string::npos)
        {
            html.insert(headEndPos, "\n    <style>\n" + customCSS + "\n    </style>");
        }

        // Aplicar prefixo base se necessário (depois das substituições)
        if (!basePrefix.empty())
        {
            html = injectBasePrefix(html, basePrefix);
            LOG_INFO("Injected base prefix: " + basePrefix);
        }
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

bool WebPortal::serveStaticFile(int clientFd, const std::string &filePath) const
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

ssize_t WebPortal::sendData(int clientFd, const void *data, size_t size) const
{
    if (m_httpServer)
    {
        return m_httpServer->sendData(clientFd, data, size);
    }
    // Fallback para send() direto se HTTPServer não estiver configurado
    return send(clientFd, data, size, MSG_NOSIGNAL);
}

std::string WebPortal::findAssetFile(const std::string &relativePath) const
{
    // Função helper para obter diretório de configuração do usuário
    auto getUserConfigDir = []() -> std::string
    {
        const char *homeDir = std::getenv("HOME");
        if (homeDir)
        {
            std::filesystem::path configDir = std::filesystem::path(homeDir) / ".config" / "retrocapture";
            return configDir.string();
        }
        return "";
    };

    // Se o caminho já é absoluto, verificar diretamente (prioridade máxima)
    std::filesystem::path testPath(relativePath);
    if (testPath.is_absolute() && std::filesystem::exists(testPath))
    {
        return std::filesystem::absolute(testPath).string();
    }

    // Extrair apenas o nome do arquivo
    std::filesystem::path inputPath(relativePath);
    std::string fileName = inputPath.filename().string();

    // Lista de locais para buscar (em ordem de prioridade)
    std::vector<std::string> possiblePaths;

    // 1. Variável de ambiente RETROCAPTURE_ASSETS_PATH (para AppImage) - PRIORIDADE MÁXIMA
    const char *assetsEnvPath = std::getenv("RETROCAPTURE_ASSETS_PATH");
    if (assetsEnvPath)
    {
        std::filesystem::path envAssetsDir(assetsEnvPath);
        possiblePaths.push_back((envAssetsDir / fileName).string());
        possiblePaths.push_back((envAssetsDir / relativePath).string());
    }

    // 2. Pasta de configuração do usuário (~/.config/retrocapture/assets/) - PRIORIDADE ALTA
    std::string userConfigDir = getUserConfigDir();
    if (!userConfigDir.empty())
    {
        std::filesystem::path userAssetsDir = std::filesystem::path(userConfigDir) / "assets";
        possiblePaths.push_back((userAssetsDir / fileName).string());
    }

    // 3. Diretório do executável/assets/ (tentar obter via /proc/self/exe no Linux)
    // No Linux, podemos usar readlink em /proc/self/exe para obter o caminho do executável
    char exePath[1024];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len != -1)
    {
        exePath[len] = '\0';
        std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        std::filesystem::path assetsDir = exeDir / "assets";
        possiblePaths.push_back((assetsDir / fileName).string());
    }

    // 4. Caminho como fornecido (pode ser relativo)
    possiblePaths.push_back(relativePath);

    // 5. Diretório atual/assets/
    possiblePaths.push_back("./assets/" + fileName);
    possiblePaths.push_back("./assets/" + relativePath);

    // 6. Diretório atual
    possiblePaths.push_back("./" + fileName);
    possiblePaths.push_back("./" + relativePath);

    // Tentar caminhos na ordem de prioridade
    for (const auto &path : possiblePaths)
    {
        std::filesystem::path fsPath(path);
        if (std::filesystem::exists(fsPath) && std::filesystem::is_regular_file(fsPath))
        {
            return std::filesystem::absolute(fsPath).string();
        }
    }

    return ""; // Não encontrado
}

void WebPortal::setColors(
    const float bg[4], const float text[4], const float primary[4],
    const float primaryLight[4], const float primaryDark[4],
    const float secondary[4], const float secondaryHighlight[4],
    const float cardHeader[4], const float border[4],
    const float success[4], const float warning[4], const float danger[4], const float info[4])
{
    // IMPORTANTE: Verificar ponteiros antes de usar memcpy para evitar corrupção de memória
    if (bg)
    {
        memcpy(m_colorBackground, bg, 4 * sizeof(float));
    }
    if (text)
    {
        memcpy(m_colorText, text, 4 * sizeof(float));
    }
    if (primary)
    {
        memcpy(m_colorPrimary, primary, 4 * sizeof(float));
    }
    if (primaryLight)
    {
        memcpy(m_colorPrimaryLight, primaryLight, 4 * sizeof(float));
    }
    if (primaryDark)
    {
        memcpy(m_colorPrimaryDark, primaryDark, 4 * sizeof(float));
    }
    if (secondary)
    {
        memcpy(m_colorSecondary, secondary, 4 * sizeof(float));
    }
    if (secondaryHighlight)
    {
        memcpy(m_colorSecondaryHighlight, secondaryHighlight, 4 * sizeof(float));
    }
    if (cardHeader)
    {
        memcpy(m_colorCardHeader, cardHeader, 4 * sizeof(float));
    }
    if (border)
    {
        memcpy(m_colorBorder, border, 4 * sizeof(float));
    }
    if (success)
    {
        memcpy(m_colorSuccess, success, 4 * sizeof(float));
    }
    if (warning)
    {
        memcpy(m_colorWarning, warning, 4 * sizeof(float));
    }
    if (danger)
    {
        memcpy(m_colorDanger, danger, 4 * sizeof(float));
    }
    if (info)
    {
        memcpy(m_colorInfo, info, 4 * sizeof(float));
    }
}

void WebPortal::setTexts(
    const std::string &streamInfo, const std::string &quickActions, const std::string &compatibility,
    const std::string &status, const std::string &codec, const std::string &resolution,
    const std::string &streamUrl, const std::string &copyUrl, const std::string &openNewTab,
    const std::string &supported, const std::string &format, const std::string &codecInfo,
    const std::string &supportedBrowsers, const std::string &formatInfo, const std::string &codecInfoValue,
    const std::string &connecting)
{
    m_textStreamInfo = streamInfo;
    m_textQuickActions = quickActions;
    m_textCompatibility = compatibility;
    m_textStatus = status;
    m_textCodec = codec;
    m_textResolution = resolution;
    m_textStreamUrl = streamUrl;
    m_textCopyUrl = copyUrl;
    m_textOpenNewTab = openNewTab;
    m_textSupported = supported;
    m_textFormat = format;
    m_textCodecInfo = codecInfo;
    m_textSupportedBrowsers = supportedBrowsers;
    m_textFormatInfo = formatInfo;
    m_textCodecInfoValue = codecInfoValue;
    m_textConnecting = connecting;
}

std::string WebPortal::generateCustomCSS(const std::string &basePrefix) const
{
    std::ostringstream css;

    // Função helper para converter float[4] (0.0-1.0) para string RGB/RGBA
    auto colorToRGBA = [](const float color[4]) -> std::string
    {
        int r = static_cast<int>(color[0] * 255.0f);
        int g = static_cast<int>(color[1] * 255.0f);
        int b = static_cast<int>(color[2] * 255.0f);
        float a = color[3];
        std::ostringstream ss;
        ss << "rgba(" << r << ", " << g << ", " << b << ", " << std::fixed << std::setprecision(2) << a << ")";
        return ss.str();
    };

    auto colorToRGB = [](const float color[4]) -> std::string
    {
        int r = static_cast<int>(color[0] * 255.0f);
        int g = static_cast<int>(color[1] * 255.0f);
        int b = static_cast<int>(color[2] * 255.0f);
        std::ostringstream ss;
        ss << "rgb(" << r << ", " << g << ", " << b << ")";
        return ss.str();
    };

    // Background do body (com imagem de fundo se especificada)
    css << "body {\n";
    css << "    background-color: " << colorToRGBA(m_colorBackground) << ";\n";

    if (!m_backgroundImagePath.empty())
    {
        // Buscar imagem de fundo nos locais padrão
        std::string foundBgPath = findAssetFile(m_backgroundImagePath);
        if (!foundBgPath.empty() && std::filesystem::exists(foundBgPath))
        {
            // Usar rota /portal-background para servir a imagem
            std::string bgUrl = (basePrefix.empty() ? "/" : basePrefix) + "portal-background";
            css << "    background-image: url('" << bgUrl << "');\n";
            css << "    background-size: cover;\n";
            css << "    background-position: center;\n";
            css << "    background-repeat: no-repeat;\n";
            css << "    background-attachment: fixed;\n";
        }
    }

    css << "}\n\n";

    // Cores CSS customizadas baseadas no styleguide RetroCapture
    css << ":root {\n";
    css << "    --primary-color: " << colorToRGB(m_colorPrimary) << ";\n";
    css << "    --primary-light: " << colorToRGB(m_colorPrimaryLight) << ";\n";
    css << "    --primary-dark: " << colorToRGB(m_colorPrimaryDark) << ";\n";
    css << "    --secondary-color: " << colorToRGB(m_colorSecondary) << ";\n";
    css << "    --secondary-highlight: " << colorToRGB(m_colorSecondaryHighlight) << ";\n";
    css << "    --success-color: " << colorToRGB(m_colorSuccess) << ";\n";
    css << "    --warning-color: " << colorToRGB(m_colorWarning) << ";\n";
    css << "    --danger-color: " << colorToRGB(m_colorDanger) << ";\n";
    css << "    --info-color: " << colorToRGB(m_colorInfo) << ";\n";
    css << "    --dark-bg: " << colorToRGB(m_colorBackground) << ";\n";
    css << "    --card-bg: " << colorToRGB(m_colorSecondary) << ";\n";
    css << "    --text-light: " << colorToRGB(m_colorText) << ";\n";
    css << "}\n\n";

    // Sobrescrever classes Bootstrap com cores customizadas
    css << ".bg-dark {\n";
    css << "    background-color: " << colorToRGBA(m_colorBackground) << " !important;\n";
    css << "}\n\n";

    css << ".text-light {\n";
    css << "    color: " << colorToRGBA(m_colorText) << " !important;\n";
    css << "}\n\n";

    css << ".text-primary {\n";
    css << "    color: " << colorToRGBA(m_colorPrimary) << " !important;\n";
    css << "}\n\n";

    css << ".bg-secondary {\n";
    css << "    background-color: " << colorToRGBA(m_colorSecondary) << " !important;\n";
    css << "}\n\n";

    css << ".card.bg-dark, .card-header.bg-dark {\n";
    css << "    background-color: " << colorToRGBA(m_colorCardHeader) << " !important;\n";
    css << "}\n\n";

    css << ".border-secondary {\n";
    css << "    border-color: " << colorToRGBA(m_colorBorder) << " !important;\n";
    css << "}\n\n";

    css << ".btn-primary {\n";
    css << "    background-color: " << colorToRGBA(m_colorPrimary) << " !important;\n";
    css << "    border-color: " << colorToRGBA(m_colorPrimary) << " !important;\n";
    css << "}\n\n";

    css << ".btn-primary:hover {\n";
    // Criar cor com transparência para hover (adicionar "dd" para 85% de opacidade em hex)
    int r = static_cast<int>(m_colorPrimary[0] * 255.0f);
    int g = static_cast<int>(m_colorPrimary[1] * 255.0f);
    int b = static_cast<int>(m_colorPrimary[2] * 255.0f);
    std::ostringstream hoverColor;
    hoverColor << "rgba(" << r << ", " << g << ", " << b << ", 0.85)";
    css << "    background-color: " << hoverColor.str() << " !important;\n";
    css << "    border-color: " << hoverColor.str() << " !important;\n";
    css << "}\n\n";

    css << ".btn-success {\n";
    css << "    background-color: " << colorToRGBA(m_colorSuccess) << " !important;\n";
    css << "    border-color: " << colorToRGBA(m_colorSuccess) << " !important;\n";
    css << "}\n\n";

    css << ".btn-success:hover {\n";
    int rSucc = static_cast<int>(m_colorSuccess[0] * 255.0f);
    int gSucc = static_cast<int>(m_colorSuccess[1] * 255.0f);
    int bSucc = static_cast<int>(m_colorSuccess[2] * 255.0f);
    std::ostringstream hoverSuccess;
    hoverSuccess << "rgba(" << rSucc << ", " << gSucc << ", " << bSucc << ", 0.85)";
    css << "    background-color: " << hoverSuccess.str() << " !important;\n";
    css << "    border-color: " << hoverSuccess.str() << " !important;\n";
    css << "}\n\n";

    css << ".badge.bg-warning {\n";
    css << "    background-color: " << colorToRGBA(m_colorWarning) << " !important;\n";
    css << "}\n\n";

    css << "code.text-primary {\n";
    css << "    color: " << colorToRGBA(m_colorPrimary) << " !important;\n";
    css << "}\n\n";

    css << ".text-muted {\n";
    css << "    color: " << colorToRGB(m_colorText) << "88 !important;\n";
    css << "}\n\n";

    // Aplicar cores do styleguide em elementos específicos
    css << ".text-info, .bi-info-circle {\n";
    css << "    color: " << colorToRGBA(m_colorInfo) << " !important;\n";
    css << "}\n\n";

    // Estilos para o novo layout moderno
    css << ".portal-header {\n";
    css << "    background-color: " << colorToRGBA(m_colorCardHeader) << " !important;\n";
    css << "    border-bottom-color: " << colorToRGBA(m_colorBorder) << " !important;\n";
    css << "}\n\n";

    css << ".portal-title {\n";
    css << "    color: " << colorToRGBA(m_colorText) << " !important;\n";
    css << "}\n\n";

    css << ".portal-subtitle {\n";
    css << "    color: " << colorToRGB(m_colorText) << "88 !important;\n";
    css << "}\n\n";

    css << ".logo-container {\n";
    css << "    background: linear-gradient(135deg, " << colorToRGB(m_colorPrimary) << ", " << colorToRGB(m_colorPrimaryLight) << ") !important;\n";
    css << "}\n\n";

    css << ".btn-icon {\n";
    css << "    background-color: " << colorToRGBA(m_colorCardHeader) << " !important;\n";
    css << "    border-color: " << colorToRGBA(m_colorBorder) << " !important;\n";
    css << "    color: " << colorToRGBA(m_colorText) << " !important;\n";
    css << "}\n\n";

    css << ".btn-icon:hover {\n";
    css << "    background-color: " << colorToRGBA(m_colorPrimary) << " !important;\n";
    css << "    border-color: " << colorToRGBA(m_colorPrimary) << " !important;\n";
    css << "}\n\n";

    css << ".stat-card {\n";
    css << "    background-color: " << colorToRGBA(m_colorCardHeader) << " !important;\n";
    css << "    border-color: " << colorToRGBA(m_colorBorder) << " !important;\n";
    css << "}\n\n";

    css << ".stat-card:hover {\n";
    css << "    border-color: " << colorToRGBA(m_colorPrimary) << " !important;\n";
    css << "    box-shadow: 0 8px 24px " << colorToRGB(m_colorPrimary) << "1a !important;\n";
    css << "}\n\n";

    css << ".stat-icon {\n";
    css << "    background: linear-gradient(135deg, " << colorToRGB(m_colorPrimaryDark) << ", " << colorToRGB(m_colorPrimary) << ") !important;\n";
    css << "    color: " << colorToRGBA(m_colorText) << " !important;\n";
    css << "}\n\n";

    css << ".stat-label {\n";
    css << "    color: " << colorToRGB(m_colorText) << "88 !important;\n";
    css << "}\n\n";

    css << ".stat-value {\n";
    css << "    color: " << colorToRGBA(m_colorText) << " !important;\n";
    css << "}\n\n";

    css << ".info-panel {\n";
    css << "    background-color: " << colorToRGBA(m_colorCardHeader) << " !important;\n";
    css << "    border-color: " << colorToRGBA(m_colorBorder) << " !important;\n";
    css << "}\n\n";

    css << ".panel-header {\n";
    css << "    background-color: " << colorToRGBA(m_colorBackground) << " !important;\n";
    css << "    border-bottom-color: " << colorToRGBA(m_colorBorder) << " !important;\n";
    css << "}\n\n";

    css << ".panel-header h5 {\n";
    css << "    color: " << colorToRGBA(m_colorText) << " !important;\n";
    css << "}\n\n";

    css << ".info-label {\n";
    css << "    color: " << colorToRGB(m_colorText) << "88 !important;\n";
    css << "}\n\n";

    css << ".info-value {\n";
    css << "    color: " << colorToRGBA(m_colorText) << " !important;\n";
    css << "}\n\n";

    css << ".info-value code {\n";
    css << "    background-color: rgba(" << static_cast<int>(m_colorPrimary[0] * 255.0f) << ", " << static_cast<int>(m_colorPrimary[1] * 255.0f) << ", " << static_cast<int>(m_colorPrimary[2] * 255.0f) << ", 0.15) !important;\n";
    css << "    border-color: rgba(" << static_cast<int>(m_colorPrimary[0] * 255.0f) << ", " << static_cast<int>(m_colorPrimary[1] * 255.0f) << ", " << static_cast<int>(m_colorPrimary[2] * 255.0f) << ", 0.3) !important;\n";
    css << "    color: " << colorToRGBA(m_colorPrimaryLight) << " !important;\n";
    css << "}\n\n";

    // Status colors
    css << ".status-connecting {\n";
    css << "    color: " << colorToRGBA(m_colorWarning) << " !important;\n";
    css << "}\n\n";

    css << ".status-playing {\n";
    css << "    color: " << colorToRGBA(m_colorSuccess) << " !important;\n";
    css << "}\n\n";

    css << ".status-error {\n";
    css << "    color: " << colorToRGBA(m_colorDanger) << " !important;\n";
    css << "}\n\n";

    css << ".status-paused {\n";
    css << "    color: " << colorToRGB(m_colorText) << "88 !important;\n";
    css << "}\n\n";

    // Efeito de brilho fosfórico (CRT Glow) nas bordas dos cards
    css << ".stat-card, .info-panel {\n";
    css << "    box-shadow: 0 0 10px " << colorToRGB(m_colorPrimary) << "1a, 0 0 20px " << colorToRGB(m_colorPrimary) << "0f !important;\n";
    css << "}\n\n";

    // Hover states com Primary Light
    css << ".stat-card:hover, .info-panel:hover {\n";
    css << "    box-shadow: 0 0 15px " << colorToRGB(m_colorPrimaryLight) << "2a, 0 0 30px " << colorToRGB(m_colorPrimaryLight) << "1a !important;\n";
    css << "}\n\n";

    return css.str();
}

void WebPortal::replaceTextInHTML(std::string &html, const std::string &oldText, const std::string &newText) const
{
    if (oldText == newText)
    {
        return; // Não precisa substituir se for igual
    }

    size_t pos = 0;
    while ((pos = html.find(oldText, pos)) != std::string::npos)
    {
        // Verificar se não está dentro de uma tag HTML (para evitar substituir atributos)
        // Procurar para trás para ver se estamos dentro de <...>
        bool insideTag = false;
        size_t checkPos = pos;
        while (checkPos > 0 && checkPos < html.length())
        {
            if (html[checkPos] == '>')
            {
                break; // Encontrou fim de tag antes do texto
            }
            if (html[checkPos] == '<')
            {
                insideTag = true;
                break; // Estamos dentro de uma tag
            }
            checkPos--;
        }

        if (!insideTag)
        {
            html.replace(pos, oldText.length(), newText);
            pos += newText.length();
        }
        else
        {
            pos += oldText.length();
        }
    }
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

std::string WebPortal::readFileContent(const std::string &filePath) const
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

std::string WebPortal::getContentType(const std::string &filePath) const
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

std::string WebPortal::extractFilePath(const std::string &request) const
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
    for (const auto &prefix : commonPrefixes)
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

std::string WebPortal::extractBasePrefix(const std::string &request) const
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

std::string WebPortal::injectBasePrefix(const std::string &html, const std::string &basePrefix) const
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

    for (const auto &[oldStr, newStr] : replacements)
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
