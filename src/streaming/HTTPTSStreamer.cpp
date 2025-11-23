#include "HTTPTSStreamer.h"
#include "../utils/Logger.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <time.h>
#include <fstream>
#include <sstream>
#include <filesystem>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

// Callback para escrever dados do MPEG-TS para os clientes HTTP
static int writeCallback(void *opaque, const uint8_t *buf, int buf_size)
{
    HTTPTSStreamer *streamer = static_cast<HTTPTSStreamer *>(opaque);
    return streamer->writeToClients(buf, buf_size);
}

HTTPTSStreamer::HTTPTSStreamer()
{
}

HTTPTSStreamer::~HTTPTSStreamer()
{
    cleanup();
}

bool HTTPTSStreamer::initialize(uint16_t port, uint32_t width, uint32_t height, uint32_t fps)
{
    // Validar parâmetros
    if (width == 0 || height == 0)
    {
        LOG_ERROR("HTTPTSStreamer::initialize: Invalid dimensions (" +
                  std::to_string(width) + "x" + std::to_string(height) + ")");
        return false;
    }
    if (fps == 0)
    {
        LOG_ERROR("HTTPTSStreamer::initialize: Invalid FPS (" + std::to_string(fps) + ")");
        return false;
    }

    m_port = port;
    m_width = width;
    m_height = height;
    m_fps = fps;

    LOG_INFO("HTTPTSStreamer::initialize: " + std::to_string(width) + "x" + std::to_string(height) +
             " @ " + std::to_string(fps) + "fps, port=" + std::to_string(port));

    return true;
}

void HTTPTSStreamer::setAudioFormat(uint32_t sampleRate, uint32_t channels)
{
    m_audioSampleRate = sampleRate;
    m_audioChannelsCount = channels;
}

void HTTPTSStreamer::setVideoCodec(const std::string &codecName)
{
    m_videoCodecName = codecName;
}

void HTTPTSStreamer::setAudioCodec(const std::string &codecName)
{
    m_audioCodecName = codecName;
}

bool HTTPTSStreamer::pushFrame(const uint8_t *data, uint32_t width, uint32_t height)
{
    if (!data || !m_active || width == 0 || height == 0)
    {
        static int logCount = 0;
        if (logCount < 3)
        {
            LOG_WARN("pushFrame: Invalid parameters (data=" + std::to_string(data != nullptr) +
                     ", active=" + std::to_string(m_active) + ", size=" +
                     std::to_string(width) + "x" + std::to_string(height) + ")");
            logCount++;
        }
        return false;
    }

    // Capturar timestamp de captura (quando o frame chega ao streamer)
    int64_t captureTimestampUs = getTimestampUs();

    // Adicionar frame com timestamp ao buffer temporal
    // OTIMIZAÇÃO: Usar move semantics para evitar cópia desnecessária
    TimestampedFrame frame;
    frame.data = std::make_shared<std::vector<uint8_t>>(width * height * 3);
    frame.data->assign(data, data + width * height * 3); // Cópia necessária, mas otimizada
    frame.width = width;
    frame.height = height;
    frame.captureTimestampUs = captureTimestampUs;
    frame.processed = false;

    {
        std::lock_guard<std::mutex> lock(m_videoBufferMutex);
        if (m_timestampedVideoBuffer.empty())
        {
            m_firstVideoTimestampUs = captureTimestampUs; // Primeiro frame define o início
        }
        m_timestampedVideoBuffer.push_back(std::move(frame));
        m_latestVideoTimestampUs = std::max(m_latestVideoTimestampUs, captureTimestampUs);

        // OTIMIZAÇÃO: Limpar dados antigos apenas ocasionalmente (não a cada frame)
        // Limpar apenas se o buffer estiver muito grande para evitar overhead
        // CRÍTICO: Reduzir frequência de cleanup para evitar perda de frames
        static size_t cleanupCounter = 0;
        // Aumentar threshold para buffer maior (30 segundos de frames baseado no FPS configurado)
        const size_t maxFramesFor30Seconds = static_cast<size_t>(m_fps * 30);
        if (++cleanupCounter >= 60 || m_timestampedVideoBuffer.size() > maxFramesFor30Seconds)
        {
            cleanupOldData();
            cleanupCounter = 0;
        }
    }

    return true;
}

bool HTTPTSStreamer::pushAudio(const int16_t *samples, size_t sampleCount)
{
    if (m_stopRequest)
    {
        return false;
    }

    if (!samples || !m_active || sampleCount == 0)
    {
        return false;
    }

    // Capturar timestamp de captura (quando o áudio chega ao streamer)
    int64_t captureTimestampUs = getTimestampUs();

    // Calcular duração deste chunk de áudio
    int64_t durationUs = (sampleCount * 1000000LL) / (m_audioSampleRate * m_audioChannelsCount);

    // Adicionar áudio com timestamp ao buffer temporal
    TimestampedAudio audio;
    audio.samples = std::make_shared<std::vector<int16_t>>(samples, samples + sampleCount);
    audio.sampleCount = sampleCount;
    audio.captureTimestampUs = captureTimestampUs;
    audio.durationUs = durationUs;
    audio.processed = false;

    {
        std::lock_guard<std::mutex> lock(m_audioBufferMutex);
        if (m_timestampedAudioBuffer.empty())
        {
            m_firstAudioTimestampUs = captureTimestampUs; // Primeiro chunk define o início
        }
        m_timestampedAudioBuffer.push_back(audio);
        m_latestAudioTimestampUs = std::max(m_latestAudioTimestampUs, captureTimestampUs);

        // Limpar dados antigos se necessário (baseado em tempo)
        cleanupOldData();
    }

    return true;
}

bool HTTPTSStreamer::start()
{
    if (m_active)
    {
        return true; // Já está ativo
    }

    // Fechar servidor anterior se existir
    m_httpServer.closeServer();
    // Aguardar um pouco para o SO liberar a porta
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Limpar recursos FFmpeg se existirem (pode ter sido parado anteriormente)
    // Fazer isso de forma segura, verificando se os recursos existem
    if (m_videoCodecContext || m_audioCodecContext || m_muxerContext)
    {
        flushCodecs();
        cleanupFFmpeg();
    }

    // Inicializar FFmpeg primeiro
    if (!initializeFFmpeg())
    {
        LOG_ERROR("Failed to initialize FFmpeg");
        return false;
    }

    // Configurar SSL se habilitado
    if (m_enableHTTPS && !m_sslCertPath.empty() && !m_sslKeyPath.empty())
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

        // Função helper para encontrar arquivo SSL em vários locais possíveis
        // Prioridade: 1) Caminho absoluto fornecido, 2) ~/.config/retrocapture/ssl/, 3) Diretório atual/ssl/
        auto findSSLFile = [getUserConfigDir](const std::string &relativePath) -> std::string
        {
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

            // 1. Pasta de configuração do usuário (~/.config/retrocapture/ssl/) - PRIORIDADE ALTA
            std::string userConfigDir = getUserConfigDir();
            if (!userConfigDir.empty())
            {
                std::filesystem::path userSSLDir = std::filesystem::path(userConfigDir) / "ssl";
                possiblePaths.push_back((userSSLDir / fileName).string());
            }

            // 2. Caminho como fornecido (pode ser relativo)
            possiblePaths.push_back(relativePath);

            // 3. Diretório atual/ssl/
            possiblePaths.push_back("./ssl/" + fileName);
            possiblePaths.push_back("./ssl/" + relativePath);

            // 4. Diretório atual
            possiblePaths.push_back("./" + fileName);
            possiblePaths.push_back("./" + relativePath);

            // 5. Um nível acima/ssl/
            possiblePaths.push_back("../ssl/" + fileName);
            possiblePaths.push_back("../ssl/" + relativePath);

            // 6. Dois níveis acima/ssl/
            possiblePaths.push_back("../../ssl/" + fileName);
            possiblePaths.push_back("../../ssl/" + relativePath);

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
        };

        // Extrair apenas o nome do arquivo do caminho fornecido
        std::filesystem::path certInputPath(m_sslCertPath);
        std::filesystem::path keyInputPath(m_sslKeyPath);
        std::string certFileName = certInputPath.filename().string();
        std::string keyFileName = keyInputPath.filename().string();

        // Tentar encontrar os arquivos
        std::string foundCertPath = findSSLFile(m_sslCertPath);
        if (foundCertPath.empty())
        {
            // Tentar apenas com o nome do arquivo
            foundCertPath = findSSLFile("ssl/" + certFileName);
        }
        if (foundCertPath.empty())
        {
            foundCertPath = findSSLFile(certFileName);
        }

        std::string foundKeyPath = findSSLFile(m_sslKeyPath);
        if (foundKeyPath.empty())
        {
            foundKeyPath = findSSLFile("ssl/" + keyFileName);
        }
        if (foundKeyPath.empty())
        {
            foundKeyPath = findSSLFile(keyFileName);
        }

        if (foundCertPath.empty())
        {
            LOG_ERROR("SSL Certificate file not found: " + m_sslCertPath);
            std::string userConfigDir = getUserConfigDir();
            if (!userConfigDir.empty())
            {
                LOG_ERROR("Searched in: ~/.config/retrocapture/ssl/, current directory, ./ssl/, ../ssl/, ../../ssl/");
            }
            else
            {
                LOG_ERROR("Searched in: current directory, ./ssl/, ../ssl/, ../../ssl/");
            }
            LOG_ERROR("Please generate certificates or disable HTTPS. Continuing with HTTP only.");
            LOG_INFO("Tip: Place certificates in ~/.config/retrocapture/ssl/ for user-specific configuration.");
            m_enableHTTPS = false;
        }
        else if (foundKeyPath.empty())
        {
            LOG_ERROR("SSL Private Key file not found: " + m_sslKeyPath);
            std::string userConfigDir = getUserConfigDir();
            if (!userConfigDir.empty())
            {
                LOG_ERROR("Searched in: ~/.config/retrocapture/ssl/, current directory, ./ssl/, ../ssl/, ../../ssl/");
            }
            else
            {
                LOG_ERROR("Searched in: current directory, ./ssl/, ../ssl/, ../../ssl/");
            }
            LOG_ERROR("Please generate certificates or disable HTTPS. Continuing with HTTP only.");
            LOG_INFO("Tip: Place certificates in ~/.config/retrocapture/ssl/ for user-specific configuration.");
            m_enableHTTPS = false;
        }
        else
        {
            LOG_INFO("Certificados SSL encontrados:");
            LOG_INFO("  Certificate: " + foundCertPath);
            LOG_INFO("  Private Key: " + foundKeyPath);
            
            // Informar se está usando certificado do diretório de configuração do usuário
            std::string userConfigDir = getUserConfigDir();
            if (!userConfigDir.empty() && 
                (foundCertPath.find(userConfigDir) != std::string::npos || 
                 foundKeyPath.find(userConfigDir) != std::string::npos))
            {
                LOG_INFO("  Using user configuration directory: ~/.config/retrocapture/ssl/");
            }

            if (!m_httpServer.setSSLCertificate(foundCertPath, foundKeyPath))
            {
                LOG_ERROR("Failed to configure SSL certificate. Continuing with HTTP only.");
                LOG_ERROR("Make sure the certificate and key files are valid PEM format.");
                m_enableHTTPS = false;
            }
            else
            {
                LOG_INFO("HTTPS configurado com sucesso");
            }
        }
    }
    else if (m_enableHTTPS)
    {
        LOG_WARN("HTTPS habilitado mas certificados não configurados. Usando HTTP.");
        LOG_WARN("Cert path: " + m_sslCertPath + ", Key path: " + m_sslKeyPath);
        m_enableHTTPS = false;
    }

    // Criar servidor HTTP/HTTPS
    if (!m_httpServer.createServer(m_port))
    {
        LOG_ERROR("Failed to create HTTP server");
        cleanupFFmpeg();
        return false;
    }

    // Configurar WebPortal para usar HTTPServer para envio de dados SSL
    m_webPortal.setHTTPServer(&m_httpServer);

    // Iniciar threads
    m_running = true;
    m_active = true;
    m_stopRequest = false;

    m_serverThread = std::thread(&HTTPTSStreamer::serverThread, this);
    m_serverThread.detach();
    m_encodingThread = std::thread(&HTTPTSStreamer::encodingThread, this);
    m_encodingThread.detach();
    m_hlsSegmentThread = std::thread(&HTTPTSStreamer::hlsSegmentThread, this);
    m_hlsSegmentThread.detach();

    std::string protocol = m_httpServer.isHTTPS() ? "HTTPS" : "HTTP";
    LOG_INFO(protocol + " TS Streamer started on port " + std::to_string(m_port));
    return true;
}

void HTTPTSStreamer::setSSLCertificatePath(const std::string &certPath, const std::string &keyPath)
{
    m_sslCertPath = certPath;
    m_sslKeyPath = keyPath;
    m_enableHTTPS = true;
}

void HTTPTSStreamer::stop()
{
    if (!m_active)
    {
        return;
    }

    m_running = false;
    m_active = false;
    m_stopRequest = true;

    // Fechar servidor HTTP/HTTPS para acordar accept()
    // IMPORTANTE: Fechar servidor ANTES de setar flags para evitar race condition
    m_httpServer.closeServer();

    // Fechar todos os sockets de clientes
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        for (int clientFd : m_clientSockets)
        {
            m_httpServer.closeClient(clientFd);
        }
        m_clientSockets.clear();
        m_clientCount = 0;
    }

    // Aguardar um tempo para threads processarem m_stopRequest e terminarem
    // Como threads são detached, precisamos aguardar um tempo razoável
    // 500ms deve ser suficiente para threads saírem dos loops
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Invalidar m_muxerContext dentro do mutex para garantir que muxPacket não use recursos inválidos
    {
        std::lock_guard<std::mutex> lock(m_muxMutex);
        m_muxerContext = nullptr; // Invalidar antes de liberar recursos
    }

    // Fazer flush dos codecs para processar pacotes pendentes
    flushCodecs();

    // Limpar recursos FFmpeg (agora seguro, pois m_muxerContext está invalidado)
    cleanupFFmpeg();

    LOG_INFO("HTTP TS Streamer stopped");
}

bool HTTPTSStreamer::isActive() const
{
    return m_active;
}

std::string HTTPTSStreamer::getStreamUrl() const
{
    return m_httpServer.getBaseUrl("localhost", m_port) + "/stream";
}

uint32_t HTTPTSStreamer::getClientCount() const
{
    return m_clientCount.load();
}

void HTTPTSStreamer::cleanup()
{
    stop();
}

void HTTPTSStreamer::handleClient(int clientFd)
{
    // Configurar socket para baixa latência
    int flag = 1;
    setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Ler requisição HTTP
    char buffer[4096];
    ssize_t bytesRead = m_httpServer.receiveData(clientFd, buffer, sizeof(buffer) - 1);
    if (bytesRead <= 0)
    {
        m_httpServer.closeClient(clientFd);
        return;
    }

    buffer[bytesRead] = '\0';
    std::string request(buffer);

    // Log da requisição para debug (temporário)
    LOG_INFO("HTTP Request received: " + request.substr(0, std::min(100UL, request.length())));

    // Se HTTPS está habilitado mas o cliente está usando HTTP, redirecionar para HTTPS
    if (m_enableHTTPS && !m_httpServer.isClientHTTPS(clientFd))
    {
        // Extrair o Host da requisição
        std::string host = "localhost";
        size_t hostPos = request.find("Host: ");
        if (hostPos != std::string::npos)
        {
            size_t hostEnd = request.find("\r\n", hostPos);
            if (hostEnd != std::string::npos)
            {
                host = request.substr(hostPos + 6, hostEnd - hostPos - 6);
                // Remover porta se presente
                size_t colonPos = host.find(':');
                if (colonPos != std::string::npos)
                {
                    host = host.substr(0, colonPos);
                }
            }
        }

        // Extrair o path da requisição
        std::string path = "/";
        size_t pathStart = request.find("GET ");
        if (pathStart != std::string::npos)
        {
            size_t pathEnd = request.find(" HTTP/", pathStart);
            if (pathEnd != std::string::npos)
            {
                path = request.substr(pathStart + 4, pathEnd - pathStart - 4);
            }
        }

        // Enviar redirecionamento 301 para HTTPS
        std::string redirectUrl = "https://" + host + ":" + std::to_string(m_port) + path;
        std::string redirectResponse =
            "HTTP/1.1 301 Moved Permanently\r\n"
            "Location: " +
            redirectUrl + "\r\n"
                          "Connection: close\r\n"
                          "\r\n";

        LOG_INFO("Redirecting HTTP to HTTPS: " + redirectUrl);
        m_httpServer.sendData(clientFd, redirectResponse.c_str(), redirectResponse.length());
        m_httpServer.closeClient(clientFd);
        return;
    }

    // Verificar tipo de requisição (ANTES de verificar portal web)
    // Isso garante que requisições de stream não sejam capturadas pelo portal
    bool isHLSPlaylist = (request.find("/stream.m3u8") != std::string::npos);
    bool isHLSSegment = (request.find("/segment_") != std::string::npos);
    bool isStreamRequest = (request.find("/stream") != std::string::npos &&
                            !isHLSPlaylist &&
                            request.find("/stream.m3u8") == std::string::npos);
    int segmentIndex = -1;

    // IMPORTANTE: Verificar portal web APENAS se NÃO for stream/HLS
    // Isso previne que stream.m3u8 seja capturado pelo portal e retorne HTML
    if (!isHLSPlaylist && !isHLSSegment && !isStreamRequest)
    {
        if (m_webPortal.isWebPortalRequest(request))
        {
            if (m_webPortal.handleRequest(clientFd, request))
            {
                m_httpServer.closeClient(clientFd);
                return;
            }
            // Se não foi processada, continuar para outras verificações
        }
    }

    // Extrair índice do segmento HLS
    if (isHLSSegment)
    {
        size_t segmentPos = request.find("/segment_");
        if (segmentPos != std::string::npos)
        {
            size_t dotPos = request.find(".ts", segmentPos);
            if (dotPos != std::string::npos)
            {
                std::string segmentStr = request.substr(segmentPos + 9, dotPos - segmentPos - 9);
                try
                {
                    segmentIndex = std::stoi(segmentStr);
                }
                catch (...)
                {
                    segmentIndex = -1;
                }
            }
        }
    }

    if (isHLSPlaylist)
    {
        // Extrair prefixo base para incluir nos segmentos do M3U8
        std::string basePrefix = "";
        size_t streamPos = request.find("/stream.m3u8");
        if (streamPos != std::string::npos)
        {
            // Extrair path antes de /stream.m3u8
            size_t pathStart = request.find("GET ");
            if (pathStart != std::string::npos)
            {
                std::string fullPath = request.substr(pathStart + 4, streamPos - pathStart - 4);
                // Remover espaços
                while (!fullPath.empty() && fullPath[0] == ' ')
                {
                    fullPath = fullPath.substr(1);
                }
                if (!fullPath.empty() && fullPath[0] == '/')
                {
                    fullPath = fullPath.substr(1);
                }
                if (!fullPath.empty())
                {
                    basePrefix = "/" + fullPath;
                }
            }
        }

        LOG_INFO("Serving HLS playlist to client (basePrefix: " + basePrefix + ")");
        serveHLSPlaylist(clientFd, basePrefix);
        m_httpServer.closeClient(clientFd);
        return;
    }
    else if (isHLSSegment && segmentIndex >= 0)
    {
        LOG_INFO("Serving HLS segment " + std::to_string(segmentIndex) + " to client");
        serveHLSSegment(clientFd, segmentIndex);
        m_httpServer.closeClient(clientFd);
        return;
    }
    else if (!isStreamRequest)
    {
        LOG_INFO("Sending 404 for request: " + request.substr(0, std::min(50UL, request.length())));
        send404(clientFd);
        m_httpServer.closeClient(clientFd);
        return;
    }

    LOG_INFO("Serving MPEG-TS stream to client");

    // Enviar headers HTTP para stream MPEG-TS
    std::ostringstream headers;
    headers << "HTTP/1.1 200 OK\r\n";
    headers << "Content-Type: video/mp2t\r\n";
    headers << "Connection: keep-alive\r\n";
    headers << "Cache-Control: no-cache\r\n";
    headers << "Pragma: no-cache\r\n";
    headers << "\r\n";

    std::string headerStr = headers.str();
    ssize_t sent = m_httpServer.sendData(clientFd, headerStr.c_str(), headerStr.length());
    if (sent < 0)
    {
        // Cliente desconectou ou erro
        m_httpServer.closeClient(clientFd);
        return;
    }

    // Enviar header do formato MPEG-TS se já foi capturado
    {
        std::lock_guard<std::mutex> headerLock(m_headerMutex);
        if (m_headerWritten && !m_formatHeader.empty())
        {
            LOG_INFO("Sending MPEG-TS format header to new client (" +
                     std::to_string(m_formatHeader.size()) + " bytes)");
            ssize_t headerSent = m_httpServer.sendData(clientFd, m_formatHeader.data(), m_formatHeader.size());
            if (headerSent < 0)
            {
                LOG_ERROR("Failed to send format header to client");
                m_httpServer.closeClient(clientFd);
                return;
            }
        }
        else
        {
            // Log removido para reduzir poluição
        }
    }

    // Adicionar cliente à lista
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        m_clientSockets.push_back(clientFd);
        m_clientCount = m_clientSockets.size();
        // Log removido para reduzir poluição
    }

    // Manter conexão aberta - dados serão enviados via writeToClients
    // Monitorar conexão para detectar desconexão
    while (!m_stopRequest && m_running)
    {
        char dummy;
        ssize_t result = m_httpServer.receiveData(clientFd, &dummy, 1);
        if (result <= 0)
        {
            break; // Cliente desconectou
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // evitar busy-waiting
    }

    // Cliente desconectou - remover da lista
    m_httpServer.closeClient(clientFd);
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        auto it = std::find(m_clientSockets.begin(), m_clientSockets.end(), clientFd);
        if (it != m_clientSockets.end())
        {
            m_clientSockets.erase(it);
            m_clientCount = m_clientSockets.size();
            // Log removido para reduzir poluição
        }
    }
}

void HTTPTSStreamer::send404(int clientFd)
{
    const char *response = "HTTP/1.1 404 Not Found\r\n"
                           "Content-Type: text/plain\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "404 Not Found";
    m_httpServer.sendData(clientFd, response, strlen(response));
}

void HTTPTSStreamer::hlsSegmentThread()
{
    LOG_INFO("HLS segment thread started");

    while (m_running && !m_stopRequest)
    {
        std::this_thread::sleep_for(std::chrono::seconds(HLS_SEGMENT_DURATION_SEC));

        if (m_stopRequest || !m_running)
        {
            break;
        }

        std::lock_guard<std::mutex> lock(m_hlsMutex);

        // Verificar se há dados suficientes no buffer
        if (m_hlsBuffer.empty())
        {
            continue;
        }

        // Criar novo segmento
        HLSSegment segment;
        segment.data = m_hlsBuffer;
        segment.index = m_hlsSegmentIndex++;
        segment.timestampUs = getTimestampUs();

        // Adicionar segmento ao buffer circular
        m_hlsSegments.push_back(segment);

        // Manter apenas os últimos HLS_SEGMENT_COUNT segmentos
        while (static_cast<int>(m_hlsSegments.size()) > HLS_SEGMENT_COUNT)
        {
            m_hlsSegments.pop_front();
        }

        // Limpar buffer para próximo segmento
        m_hlsBuffer.clear();

        LOG_INFO("HLS segment " + std::to_string(segment.index) + " created (" +
                 std::to_string(segment.data.size()) + " bytes)");
    }

    LOG_INFO("HLS segment thread stopped");
}

std::string HTTPTSStreamer::generateM3U8Playlist(const std::string &basePrefix) const
{
    std::lock_guard<std::mutex> lock(m_hlsMutex);

    std::ostringstream m3u8;
    m3u8 << "#EXTM3U\n";
    m3u8 << "#EXT-X-VERSION:3\n";
    m3u8 << "#EXT-X-TARGETDURATION:" << HLS_SEGMENT_DURATION_SEC << "\n";
    m3u8 << "#EXT-X-MEDIA-SEQUENCE:" << (m_hlsSegmentIndex - static_cast<int>(m_hlsSegments.size())) << "\n";

    std::string segmentPrefix = basePrefix.empty() ? "" : basePrefix;
    if (!segmentPrefix.empty() && segmentPrefix.back() != '/')
    {
        segmentPrefix += "/";
    }

    for (const auto &segment : m_hlsSegments)
    {
        m3u8 << "#EXTINF:" << HLS_SEGMENT_DURATION_SEC << ".0,\n";
        m3u8 << segmentPrefix << "segment_" << segment.index << ".ts\n";
    }

    return m3u8.str();
}

void HTTPTSStreamer::serveHLSPlaylist(int clientFd, const std::string &basePrefix)
{
    std::string playlist = generateM3U8Playlist(basePrefix);

    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: application/vnd.apple.mpegurl\r\n";
    response << "Content-Length: " << playlist.length() << "\r\n";
    response << "Connection: close\r\n";
    response << "Cache-Control: no-cache\r\n";
    response << "\r\n";
    response << playlist;

    std::string responseStr = response.str();
    m_httpServer.sendData(clientFd, responseStr.c_str(), responseStr.length());
}

void HTTPTSStreamer::serveHLSSegment(int clientFd, int segmentIndex)
{
    std::lock_guard<std::mutex> lock(m_hlsMutex);

    // Procurar segmento pelo índice
    for (const auto &segment : m_hlsSegments)
    {
        if (segment.index == segmentIndex)
        {
            std::ostringstream response;
            response << "HTTP/1.1 200 OK\r\n";
            response << "Content-Type: video/mp2t\r\n";
            response << "Content-Length: " << segment.data.size() << "\r\n";
            response << "Connection: close\r\n";
            response << "Cache-Control: public, max-age=3600\r\n";
            response << "\r\n";

            std::string headerStr = response.str();
            m_httpServer.sendData(clientFd, headerStr.c_str(), headerStr.length());
            m_httpServer.sendData(clientFd, segment.data.data(), segment.data.size());
            return;
        }
    }

    // Segmento não encontrado
    send404(clientFd);
}

int HTTPTSStreamer::writeToClients(const uint8_t *buf, int buf_size)
{
    if (!buf || buf_size <= 0 || m_stopRequest)
    {
        return buf_size; // Retornar sucesso para não bloquear FFmpeg
    }

    // Armazenar header do formato se ainda não foi armazenado
    {
        std::lock_guard<std::mutex> headerLock(m_headerMutex);
        if (!m_headerWritten && buf_size > 0)
        {
            // Primeiros bytes são geralmente o header do formato
            // Para MPEG-TS, o header geralmente tem alguns KB
            if (buf_size < 64 * 1024) // Se for menor que 64KB, provavelmente é o header
            {
                m_formatHeader.assign(buf, buf + buf_size);
                m_headerWritten = true;
                LOG_INFO("MPEG-TS format header captured (" + std::to_string(buf_size) + " bytes)");
            }
        }
    }

    // Acumular dados para HLS
    {
        std::lock_guard<std::mutex> hlsLock(m_hlsMutex);
        m_hlsBuffer.insert(m_hlsBuffer.end(), buf, buf + buf_size);
    }

    // Enviar dados diretamente para todos os clientes (sem buffer, sem sincronização)
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);

        // Verificar novamente após adquirir o lock
        if (m_stopRequest || m_clientSockets.empty())
        {
            // Sem clientes conectados ou parada solicitada, retornar sucesso para não bloquear o FFmpeg
            return buf_size;
        }

        // CRÍTICO: writeCallback é chamado dentro do lock do muxing
        // Deve ser MUITO rápido para não bloquear o encoding
        // IMPORTANTE: Retornar apenas buf_size se TODOS os clientes receberam TODOS os dados
        // Retornar menos que buf_size faz o FFmpeg tentar novamente, o que pode causar corrupção

        // Enviar dados para todos os clientes conectados
        // Remover clientes que falharam ao enviar
        size_t minSent = static_cast<size_t>(buf_size); // Rastrear mínimo enviado a todos os clientes
        auto it = m_clientSockets.begin();
        while (it != m_clientSockets.end())
        {
            int clientFd = *it;
            size_t totalSent = 0;

            // Tentar enviar tudo de uma vez (sem loop de retries para ser mais rápido)
            size_t toSend = static_cast<size_t>(buf_size);
            // Limitar tamanho do chunk para evitar problemas
            if (toSend > 65536)
            {
                toSend = 65536;
            }

            ssize_t sent = m_httpServer.sendData(clientFd, buf, toSend);
            if (sent < 0)
            {
                // Erro ao enviar - remover cliente
                m_httpServer.closeClient(clientFd);
                it = m_clientSockets.erase(it);
                m_clientCount = m_clientSockets.size();
            }
            else if (sent == 0)
            {
                // Cliente fechou a conexão
                m_httpServer.closeClient(clientFd);
                it = m_clientSockets.erase(it);
                m_clientCount = m_clientSockets.size();
            }
            else
            {
                totalSent = static_cast<size_t>(sent);
                ++it;
            }

            // Atualizar mínimo enviado (se este cliente recebeu menos, usar esse valor)
            if (totalSent < minSent)
            {
                minSent = totalSent;
            }
        }

        // CRÍTICO: Sempre retornar buf_size para não bloquear o FFmpeg
        // O buffer AVIO de 1MB é suficiente para lidar com pequenos atrasos de envio
        // Retornar menos que buf_size causa retries do FFmpeg que quebram sincronização e causam corrupção
        // Se alguns clientes não receberam todos os dados, o buffer AVIO vai lidar com isso
        return buf_size;
    }
}

bool HTTPTSStreamer::initializeFFmpeg()
{
    // Initialize the FFmpeg codecs
    if (!initializeVideoCodec())
    {
        LOG_ERROR("Failed to initialize video codec");
        return false;
    }
    if (!initializeAudioCodec())
    {
        LOG_ERROR("Failed to initialize audio codec");
        return false;
    }
    // Initialize the FFmpeg muxers
    if (!initializeMuxers())
    {
        LOG_ERROR("Failed to initialize muxers");
        return false;
    }
    return true;
}

bool HTTPTSStreamer::initializeVideoCodec()
{
    // Initialize the video codec
    // Tentar encontrar codec por nome primeiro, depois por ID
    const AVCodec *codec = nullptr;

    if (m_videoCodecName == "h264" || m_videoCodecName == "libx264")
    {
        // Para H.264, tentar libx264 primeiro (encoder mais comum)
        codec = avcodec_find_encoder_by_name("libx264");
        if (!codec)
        {
            // Fallback: tentar encontrar por ID
            codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        }
        if (!codec)
        {
            LOG_ERROR("H.264 codec not found. Make sure libx264 is installed.");
            return false;
        }
    }
    else if (m_videoCodecName == "h265" || m_videoCodecName == "libx265" || m_videoCodecName == "hevc")
    {
        // Para H.265, tentar libx265 primeiro (encoder software mais comum)
        codec = avcodec_find_encoder_by_name("libx265");
        if (!codec)
        {
            // Fallback: tentar encontrar por ID
            codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
        }
        if (!codec)
        {
            LOG_ERROR("H.265 codec not found. Make sure libx265 is installed.");
            return false;
        }
    }
    else if (m_videoCodecName == "vp8" || m_videoCodecName == "libvpx-vp8")
    {
        // Para VP8, tentar libvpx-vp8 primeiro
        codec = avcodec_find_encoder_by_name("libvpx-vp8");
        if (!codec)
        {
            // Fallback: tentar encontrar por ID
            codec = avcodec_find_encoder(AV_CODEC_ID_VP8);
        }
        if (!codec)
        {
            LOG_ERROR("VP8 codec not found. Make sure libvpx is installed.");
            return false;
        }
    }
    else if (m_videoCodecName == "vp9" || m_videoCodecName == "libvpx-vp9")
    {
        // Para VP9, tentar libvpx-vp9 primeiro
        codec = avcodec_find_encoder_by_name("libvpx-vp9");
        if (!codec)
        {
            // Fallback: tentar encontrar por ID
            codec = avcodec_find_encoder(AV_CODEC_ID_VP9);
        }
        if (!codec)
        {
            LOG_ERROR("VP9 codec not found. Make sure libvpx is installed.");
            return false;
        }
    }
    else
    {
        // Para outros codecs, tentar por nome
        codec = avcodec_find_encoder_by_name(m_videoCodecName.c_str());
        if (!codec)
        {
            LOG_ERROR("Video codec " + m_videoCodecName + " not found");
            return false;
        }
    }
    AVCodecContext *codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx)
    {
        LOG_ERROR("Failed to allocate video codec context");
        return false;
    }
    codecCtx->codec_id = codec->id;
    codecCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    codecCtx->width = m_width;
    codecCtx->height = m_height;
    codecCtx->time_base = {1, static_cast<int>(m_fps)};
    codecCtx->framerate = {static_cast<int>(m_fps), 1};
    codecCtx->gop_size = static_cast<int>(m_fps * 2); // Keyframe a cada 2 segundos (melhor que 1 frame)
    codecCtx->max_b_frames = 0;                       // Sem B-frames para baixa latência
    codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    codecCtx->bit_rate = m_videoBitrate;
    codecCtx->thread_count = 0;                                // Auto-detect (melhor que fixo)
    codecCtx->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME; // Usar threading de slice e frame para paralelismo

    // Para codecs que precisam de global header (H.264, H.265, VP8, VP9)
    if (codec->id == AV_CODEC_ID_H264 || codec->id == AV_CODEC_ID_HEVC ||
        codec->id == AV_CODEC_ID_VP8 || codec->id == AV_CODEC_ID_VP9)
    {
        codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Configurar preset rápido para libx264/libx265 (velocidade de encoding)
    AVDictionary *opts = nullptr;
    if (codec->id == AV_CODEC_ID_H264)
    {
        // Preset configurável via UI (padrão: "veryfast" para máxima velocidade)
        av_dict_set(&opts, "preset", m_h264Preset.c_str(), 0);
        // Tune "zerolatency" para streaming em tempo real (remove delay do codec)
        av_dict_set(&opts, "tune", "zerolatency", 0);
        // Profile "baseline" para compatibilidade máxima
        av_dict_set(&opts, "profile", "baseline", 0);
        // Keyframe mínimo e máximo a cada 2 segundos (garantir keyframes periódicos)
        int keyint = static_cast<int>(m_fps * 2);
        av_dict_set_int(&opts, "keyint_min", keyint, 0);
        av_dict_set_int(&opts, "keyint", keyint, 0); // Intervalo máximo de keyframes (igual ao mínimo para forçar)
        // OTIMIZAÇÃO: Reduzir lookahead para zero (zerolatency já faz isso, mas garantir)
        av_dict_set_int(&opts, "rc-lookahead", 0, 0);
        // OTIMIZAÇÃO: Reduzir buffer de VBV para menor latência
        av_dict_set_int(&opts, "vbv-bufsize", m_videoBitrate / 10, 0); // 100ms de buffer
        // Desabilitar scenecut para garantir keyframes exatos no intervalo especificado
        av_dict_set_int(&opts, "scenecut", 0, 0);
    }
    else if (codec->id == AV_CODEC_ID_HEVC)
    {
        // Preset configurável via UI (padrão: "veryfast" para máxima velocidade)
        av_dict_set(&opts, "preset", m_h265Preset.c_str(), 0);
        // Tune "zerolatency" para streaming em tempo real (remove delay do codec)
        av_dict_set(&opts, "tune", "zerolatency", 0);
        // Profile configurável via UI (padrão: "main" para compatibilidade máxima)
        av_dict_set(&opts, "profile", m_h265Profile.c_str(), 0);
        // Level configurável via UI (padrão: "auto" para detecção automática)
        if (m_h265Level != "auto" && !m_h265Level.empty())
        {
            av_dict_set(&opts, "level-idc", m_h265Level.c_str(), 0);
        }
        // Keyframe mínimo e máximo a cada 2 segundos (garantir keyframes periódicos)
        int keyint = static_cast<int>(m_fps * 2);
        av_dict_set_int(&opts, "keyint_min", keyint, 0);
        av_dict_set_int(&opts, "keyint", keyint, 0); // Intervalo máximo de keyframes (igual ao mínimo para forçar)
        // OTIMIZAÇÃO: Reduzir lookahead para zero (zerolatency já faz isso, mas garantir)
        av_dict_set_int(&opts, "rc-lookahead", 0, 0);
        // OTIMIZAÇÃO: Reduzir buffer de VBV para menor latência
        av_dict_set_int(&opts, "vbv-bufsize", m_videoBitrate / 10, 0); // 100ms de buffer
        // Desabilitar scenecut para garantir keyframes exatos no intervalo especificado
        av_dict_set_int(&opts, "scenecut", 0, 0);
    }
    else if (codec->id == AV_CODEC_ID_VP8)
    {
        // Speed/Quality: 0-16 (0 = melhor qualidade, 16 = mais rápido)
        // Para streaming, usar valores mais altos (mais rápido)
        av_dict_set_int(&opts, "speed", m_vp8Speed, 0);
        // Deadline "realtime" para streaming em tempo real (prioriza velocidade)
        av_dict_set(&opts, "deadline", "realtime", 0);
        // Lag-in-frames: 0 para baixa latência (sem lookahead)
        av_dict_set_int(&opts, "lag-in-frames", 0, 0);
        // Keyframe mínimo e máximo a cada 2 segundos
        int keyint = static_cast<int>(m_fps * 2);
        av_dict_set_int(&opts, "keyint_min", keyint, 0);
        av_dict_set_int(&opts, "keyint_max", keyint, 0);
        // Threads para paralelismo
        av_dict_set_int(&opts, "threads", 0, 0); // Auto-detect
    }
    else if (codec->id == AV_CODEC_ID_VP9)
    {
        // Speed/Quality: 0-9 (0 = melhor qualidade, 9 = mais rápido)
        // Para streaming, usar valores mais altos (mais rápido)
        av_dict_set_int(&opts, "speed", m_vp9Speed, 0);
        // Deadline "realtime" para streaming em tempo real (prioriza velocidade)
        av_dict_set(&opts, "deadline", "realtime", 0);
        // Lag-in-frames: 0 para baixa latência (sem lookahead)
        av_dict_set_int(&opts, "lag-in-frames", 0, 0);
        // Keyframe mínimo e máximo a cada 2 segundos
        int keyint = static_cast<int>(m_fps * 2);
        av_dict_set_int(&opts, "keyint_min", keyint, 0);
        av_dict_set_int(&opts, "keyint_max", keyint, 0);
        // Threads para paralelismo
        av_dict_set_int(&opts, "threads", 0, 0); // Auto-detect
        // Tile columns para paralelismo (melhora performance)
        av_dict_set_int(&opts, "tile-columns", 2, 0);
    }

    // Abrir codec com opções
    if (avcodec_open2(codecCtx, codec, &opts) < 0)
    {
        LOG_ERROR("Failed to open video codec");
        av_dict_free(&opts);
        avcodec_free_context(&codecCtx);
        return false;
    }
    av_dict_free(&opts); // Liberar opções após abrir codec

    m_videoCodecContext = codecCtx;

    // IMPORTANTE: NÃO criar SWS context aqui - será criado dinamicamente em convertRGBToYUV
    // quando recebermos frames com dimensões conhecidas (pode ser diferente de m_width x m_height)
    // Isso permite que frames de qualquer tamanho sejam redimensionados para m_width x m_height
    // durante a conversão RGB->YUV, tudo em um único passo otimizado
    m_swsContext = nullptr;
    m_swsSrcWidth = 0;
    m_swsSrcHeight = 0;
    m_swsDstWidth = 0;
    m_swsDstHeight = 0;

    // Alocar frame de vídeo
    AVFrame *videoFrame = av_frame_alloc();
    if (!videoFrame)
    {
        LOG_ERROR("Failed to allocate video frame");
        avcodec_free_context(&codecCtx);
        m_videoCodecContext = nullptr;
        return false;
    }

    videoFrame->format = AV_PIX_FMT_YUV420P;
    videoFrame->width = m_width;
    videoFrame->height = m_height;
    if (av_frame_get_buffer(videoFrame, 0) < 0)
    {
        LOG_ERROR("Failed to allocate video frame buffer");
        av_frame_free(&videoFrame);
        avcodec_free_context(&codecCtx);
        m_videoCodecContext = nullptr;
        return false;
    }
    m_videoFrame = videoFrame;

    return true;
}

bool HTTPTSStreamer::initializeAudioCodec()
{
    // Initialize the audio codec
    // Tentar encontrar codec por nome primeiro, depois por ID
    const AVCodec *codec = nullptr;

    if (m_audioCodecName == "aac")
    {
        // Para AAC, tentar libfdk_aac primeiro (melhor qualidade), depois aac
        codec = avcodec_find_encoder_by_name("libfdk_aac");
        if (!codec)
        {
            codec = avcodec_find_encoder_by_name("aac");
        }
        if (!codec)
        {
            // Fallback: tentar encontrar por ID
            codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        }
        if (!codec)
        {
            LOG_ERROR("AAC codec not found. Make sure libfdk-aac or aac encoder is available.");
            return false;
        }
    }
    else
    {
        // Para outros codecs, tentar por nome
        codec = avcodec_find_encoder_by_name(m_audioCodecName.c_str());
        if (!codec)
        {
            LOG_ERROR("Audio codec " + m_audioCodecName + " not found");
            return false;
        }
    }
    AVCodecContext *codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx)
    {
        LOG_ERROR("Failed to allocate audio codec context");
        return false;
    }
    codecCtx->codec_id = codec->id;
    codecCtx->codec_type = AVMEDIA_TYPE_AUDIO;
    codecCtx->sample_rate = m_audioSampleRate;
    av_channel_layout_default(&codecCtx->ch_layout, m_audioChannelsCount);
    // AAC requer float planar (fltp), não s16
    codecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    codecCtx->bit_rate = m_audioBitrate;
    codecCtx->thread_count = 4;
    codecCtx->time_base = {1, static_cast<int>(m_audioSampleRate)};

    // Abrir codec
    if (avcodec_open2(codecCtx, codec, nullptr) < 0)
    {
        LOG_ERROR("Failed to open audio codec");
        avcodec_free_context(&codecCtx);
        return false;
    }

    m_audioCodecContext = codecCtx;

    // Criar SWR context para conversão int16 -> float planar
    SwrContext *swrCtx = swr_alloc();
    if (!swrCtx)
    {
        LOG_ERROR("Failed to allocate SWR context");
        avcodec_free_context(&codecCtx);
        m_audioCodecContext = nullptr;
        return false;
    }

    AVChannelLayout inChLayout, outChLayout;
    av_channel_layout_default(&inChLayout, m_audioChannelsCount);
    av_channel_layout_default(&outChLayout, m_audioChannelsCount);

    av_opt_set_chlayout(swrCtx, "in_chlayout", &inChLayout, 0);
    av_opt_set_int(swrCtx, "in_sample_rate", static_cast<int>(m_audioSampleRate), 0);
    av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    av_opt_set_chlayout(swrCtx, "out_chlayout", &outChLayout, 0);
    av_opt_set_int(swrCtx, "out_sample_rate", static_cast<int>(m_audioSampleRate), 0);
    av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);

    if (swr_init(swrCtx) < 0)
    {
        LOG_ERROR("Failed to initialize SWR context");
        av_channel_layout_uninit(&inChLayout);
        av_channel_layout_uninit(&outChLayout);
        swr_free(&swrCtx);
        avcodec_free_context(&codecCtx);
        m_audioCodecContext = nullptr;
        return false;
    }

    av_channel_layout_uninit(&inChLayout);
    av_channel_layout_uninit(&outChLayout);
    m_swrContext = swrCtx;

    // Alocar frame de áudio
    AVFrame *audioFrame = av_frame_alloc();
    if (!audioFrame)
    {
        LOG_ERROR("Failed to allocate audio frame");
        swr_free(&swrCtx);
        avcodec_free_context(&codecCtx);
        m_audioCodecContext = nullptr;
        m_swrContext = nullptr;
        return false;
    }

    audioFrame->format = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&audioFrame->ch_layout, m_audioChannelsCount);
    audioFrame->sample_rate = m_audioSampleRate;
    audioFrame->nb_samples = codecCtx->frame_size;
    if (av_frame_get_buffer(audioFrame, 0) < 0)
    {
        LOG_ERROR("Failed to allocate audio frame buffer");
        av_frame_free(&audioFrame);
        swr_free(&swrCtx);
        avcodec_free_context(&codecCtx);
        m_audioCodecContext = nullptr;
        m_swrContext = nullptr;
        return false;
    }
    m_audioFrame = audioFrame;

    return true;
}

bool HTTPTSStreamer::initializeMuxers()
{
    AVCodecContext *videoCtx = static_cast<AVCodecContext *>(m_videoCodecContext);
    AVCodecContext *audioCtx = static_cast<AVCodecContext *>(m_audioCodecContext);

    if (!videoCtx || !audioCtx)
    {
        LOG_ERROR("Codecs must be initialized before muxers");
        return false;
    }

    // Initialize the muxers
    AVFormatContext *formatCtx = avformat_alloc_context();
    if (!formatCtx)
    {
        LOG_ERROR("Failed to allocate muxer context");
        return false;
    }
    formatCtx->oformat = av_guess_format("mpegts", nullptr, nullptr);
    if (!formatCtx->oformat)
    {
        LOG_ERROR("Failed to guess muxer format");
        avformat_free_context(formatCtx);
        return false;
    }
    formatCtx->url = av_strdup("pipe:");
    if (!formatCtx->url)
    {
        LOG_ERROR("Failed to allocate muxer URL");
        avformat_free_context(formatCtx);
        return false;
    }

    // Criar stream de vídeo
    AVStream *videoStream = avformat_new_stream(formatCtx, nullptr);
    if (!videoStream)
    {
        LOG_ERROR("Failed to create video stream");
        av_free(const_cast<char *>(formatCtx->url));
        avformat_free_context(formatCtx);
        return false;
    }
    videoStream->id = formatCtx->nb_streams - 1;

    // Copiar parâmetros do codec para o stream
    if (avcodec_parameters_from_context(videoStream->codecpar, videoCtx) < 0)
    {
        LOG_ERROR("Failed to copy video codec parameters");
        av_free(const_cast<char *>(formatCtx->url));
        avformat_free_context(formatCtx);
        return false;
    }

    // Garantir que codec_type e codec_id estão corretos (importante para VP8/VP9)
    videoStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    videoStream->codecpar->codec_id = videoCtx->codec_id;

    m_videoStream = videoStream;

    // Criar stream de áudio
    AVStream *audioStream = avformat_new_stream(formatCtx, nullptr);
    if (!audioStream)
    {
        LOG_ERROR("Failed to create audio stream");
        av_free(const_cast<char *>(formatCtx->url));
        avformat_free_context(formatCtx);
        return false;
    }
    audioStream->id = formatCtx->nb_streams - 1;

    // Copiar parâmetros do codec para o stream
    if (avcodec_parameters_from_context(audioStream->codecpar, audioCtx) < 0)
    {
        LOG_ERROR("Failed to copy audio codec parameters");
        av_free(const_cast<char *>(formatCtx->url));
        avformat_free_context(formatCtx);
        return false;
    }

    // Garantir que codec_type e codec_id estão corretos
    audioStream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    audioStream->codecpar->codec_id = audioCtx->codec_id;

    m_audioStream = audioStream;

    // Configurar callback de escrita
    // Aumentar buffer para 1MB para reduzir chamadas ao callback e evitar corrupção
    // Buffer maior = menos fragmentação = menos chance de corrupção
    // Buffer grande também permite que writeCallback seja mais rápido (pode pular clientes lentos)
    formatCtx->pb = avio_alloc_context(
        static_cast<unsigned char *>(av_malloc(1024 * 1024)), 1024 * 1024,
        1, this, nullptr, writeCallback, nullptr);
    if (!formatCtx->pb)
    {
        LOG_ERROR("Failed to allocate AVIO context");
        av_free(const_cast<char *>(formatCtx->url));
        avformat_free_context(formatCtx);
        return false;
    }

    // Configurar time_base dos streams explicitamente
    videoStream->time_base = videoCtx->time_base;
    audioStream->time_base = audioCtx->time_base;

    LOG_INFO("Video stream time_base: " + std::to_string(videoStream->time_base.num) + "/" +
             std::to_string(videoStream->time_base.den));
    LOG_INFO("Audio stream time_base: " + std::to_string(audioStream->time_base.num) + "/" +
             std::to_string(audioStream->time_base.den));
    LOG_INFO("Audio sample_rate: " + std::to_string(m_audioSampleRate));

    // Para VP8/VP9, enviar um frame dummy para gerar extradata ANTES de escrever o header
    // VP8/VP9 não geram extradata automaticamente como H.264, precisamos forçar
    if (videoCtx->codec_id == AV_CODEC_ID_VP8 || videoCtx->codec_id == AV_CODEC_ID_VP9)
    {
        AVFrame *dummyFrame = av_frame_alloc();
        if (dummyFrame)
        {
            dummyFrame->format = videoCtx->pix_fmt;
            dummyFrame->width = videoCtx->width;
            dummyFrame->height = videoCtx->height;
            if (av_frame_get_buffer(dummyFrame, 32) >= 0)
            {
                // Preencher com dados YUV válidos (frame preto)
                // Y = 0 (preto), U = 128 (neutro), V = 128 (neutro)
                memset(dummyFrame->data[0], 0, dummyFrame->linesize[0] * dummyFrame->height);
                if (dummyFrame->data[1])
                    memset(dummyFrame->data[1], 128, dummyFrame->linesize[1] * dummyFrame->height / 2);
                if (dummyFrame->data[2])
                    memset(dummyFrame->data[2], 128, dummyFrame->linesize[2] * dummyFrame->height / 2);

                dummyFrame->pts = 0;
                dummyFrame->flags |= AV_FRAME_FLAG_KEY;

                // Enviar frame dummy para gerar extradata
                if (avcodec_send_frame(videoCtx, dummyFrame) >= 0)
                {
                    AVPacket *pkt = av_packet_alloc();
                    if (pkt)
                    {
                        // Receber e descartar pacotes gerados
                        while (avcodec_receive_packet(videoCtx, pkt) >= 0)
                        {
                            av_packet_unref(pkt);
                        }
                        av_packet_free(&pkt);
                    }

                    // Atualizar codecpar ANTES de escrever o header
                    if (avcodec_parameters_from_context(videoStream->codecpar, videoCtx) >= 0)
                    {
                        // Garantir novamente que codec_type e codec_id estão corretos
                        videoStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
                        videoStream->codecpar->codec_id = videoCtx->codec_id;
                    }
                }
            }
            av_frame_free(&dummyFrame);
        }
    }

    // Escrever header do formato
    if (avformat_write_header(formatCtx, nullptr) < 0)
    {
        LOG_ERROR("Failed to write format header");
        avio_context_free(&formatCtx->pb);
        av_free(const_cast<char *>(formatCtx->url));
        avformat_free_context(formatCtx);
        return false;
    }

    m_muxerContext = formatCtx;
    return true;
}

void HTTPTSStreamer::cleanupFFmpeg()
{
    // Tornar esta função idempotente - pode ser chamada múltiplas vezes sem problemas
    // Verificar se já está limpo para evitar double free
    if (!m_videoCodecContext && !m_audioCodecContext && !m_muxerContext &&
        !m_swsContext && !m_swrContext && !m_videoFrame && !m_audioFrame)
    {
        return; // Já está limpo
    }

    AVCodecContext *videoCtx = static_cast<AVCodecContext *>(m_videoCodecContext);
    AVCodecContext *audioCtx = static_cast<AVCodecContext *>(m_audioCodecContext);
    AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_muxerContext);
    SwsContext *swsCtx = static_cast<SwsContext *>(m_swsContext);
    SwrContext *swrCtx = static_cast<SwrContext *>(m_swrContext);
    AVFrame *videoFrame = static_cast<AVFrame *>(m_videoFrame);
    AVFrame *audioFrame = static_cast<AVFrame *>(m_audioFrame);

    // Limpar streams (não precisam ser liberados, são gerenciados pelo formatCtx)
    m_videoStream = nullptr;
    m_audioStream = nullptr;

    // Limpar acumulador de áudio
    {
        std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
        m_audioAccumulator.clear();
    }

    // Resetar rastreamento de PTS/DTS e contadores
    {
        std::lock_guard<std::mutex> lock(m_ptsMutex);
        m_lastVideoFramePTS = -1;
        m_lastVideoPTS = -1;
        m_lastVideoDTS = -1;
        m_lastAudioFramePTS = -1;
        m_lastAudioPTS = -1;
        m_lastAudioDTS = -1;
    }
    m_videoFrameCount = 0;

    // Limpar header do formato
    {
        std::lock_guard<std::mutex> headerLock(m_headerMutex);
        m_formatHeader.clear();
        m_headerWritten = false;
    }

    if (swrCtx)
    {
        swr_free(&swrCtx);
        m_swrContext = nullptr;
    }

    if (swsCtx)
    {
        sws_freeContext(swsCtx);
        m_swsContext = nullptr;
        m_swsSrcWidth = 0;
        m_swsSrcHeight = 0;
        m_swsDstWidth = 0;
        m_swsDstHeight = 0;
    }

    if (audioFrame)
    {
        av_frame_free(&audioFrame);
        m_audioFrame = nullptr;
    }

    if (videoFrame)
    {
        av_frame_free(&videoFrame);
        m_videoFrame = nullptr;
    }

    if (videoCtx)
    {
        AVCodecContext *ctx = videoCtx;
        m_videoCodecContext = nullptr; // Marcar como nullptr ANTES de liberar
        avcodec_free_context(&ctx);
    }
    if (audioCtx)
    {
        AVCodecContext *ctx = audioCtx;
        m_audioCodecContext = nullptr; // Marcar como nullptr ANTES de liberar
        avcodec_free_context(&ctx);
    }
    if (formatCtx)
    {
        // Marcar como nullptr ANTES de liberar para evitar uso após liberação
        m_muxerContext = nullptr;

        // Escrever trailer antes de fechar (apenas se o formato ainda for válido)
        // formatCtx pode ter sido parcialmente destruído, então verificar antes
        if (formatCtx->oformat && formatCtx->pb)
        {
            av_write_trailer(formatCtx);
        }

        // Liberar AVIO context
        if (formatCtx->pb)
        {
            avio_context_free(&formatCtx->pb);
        }

        if (formatCtx->url)
        {
            av_free(const_cast<char *>(formatCtx->url));
        }
        avformat_free_context(formatCtx);
    }
}

void HTTPTSStreamer::flushCodecs()
{
    AVCodecContext *videoCtx = static_cast<AVCodecContext *>(m_videoCodecContext);
    AVCodecContext *audioCtx = static_cast<AVCodecContext *>(m_audioCodecContext);
    AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_muxerContext);

    if (videoCtx)
    {
        avcodec_flush_buffers(videoCtx);
    }
    if (audioCtx)
    {
        avcodec_flush_buffers(audioCtx);
    }
    if (formatCtx)
    {
        avformat_flush(formatCtx);
    }
}

bool HTTPTSStreamer::convertRGBToYUV(const uint8_t *rgbData, uint32_t width, uint32_t height, void *videoFrame)
{
    if (!rgbData || !videoFrame || width == 0 || height == 0)
    {
        return false;
    }

    AVFrame *frame = static_cast<AVFrame *>(videoFrame);
    AVCodecContext *codecCtx = static_cast<AVCodecContext *>(m_videoCodecContext);

    if (!frame || !codecCtx)
    {
        return false;
    }

    if (av_frame_make_writable(frame) < 0)
    {
        return false;
    }

    // Obter dimensões de destino (dimensões configuradas para streaming)
    uint32_t dstWidth = m_width;
    uint32_t dstHeight = m_height;

    // Validar dimensões de destino (não podem ser 0)
    if (dstWidth == 0 || dstHeight == 0)
    {
        LOG_ERROR("convertRGBToYUV: Invalid destination dimensions (" +
                  std::to_string(dstWidth) + "x" + std::to_string(dstHeight) + ")");
        return false;
    }

    // Obter SwsContext atual (pode ser nullptr se ainda não foi criado ou se dimensões mudaram)
    SwsContext *swsCtx = static_cast<SwsContext *>(m_swsContext);

    // Verificar se precisamos recriar o SwsContext (dimensões de entrada mudaram ou não existe)
    if (!swsCtx ||
        m_swsSrcWidth != width ||
        m_swsSrcHeight != height ||
        m_swsDstWidth != dstWidth ||
        m_swsDstHeight != dstHeight)
    {
        // Liberar contexto antigo se existir
        if (swsCtx)
        {
            sws_freeContext(swsCtx);
            swsCtx = nullptr;
        }

        // Criar novo SwsContext que faz resize + conversão em um único passo
        // RGB (dimensões de entrada) -> YUV420P (dimensões de streaming)
        swsCtx = sws_getContext(
            width, height, AV_PIX_FMT_RGB24,
            dstWidth, dstHeight, AV_PIX_FMT_YUV420P,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

        if (!swsCtx)
        {
            LOG_ERROR("Failed to create SWS context for resize+conversion: " +
                      std::to_string(width) + "x" + std::to_string(height) + " -> " +
                      std::to_string(dstWidth) + "x" + std::to_string(dstHeight));
            return false;
        }

        m_swsContext = swsCtx;
        m_swsSrcWidth = width;
        m_swsSrcHeight = height;
        m_swsDstWidth = dstWidth;
        m_swsDstHeight = dstHeight;
    }

    // Fazer resize + conversão RGB->YUV em um único passo
    const uint8_t *srcData[1] = {rgbData};
    int srcLinesize[1] = {static_cast<int>(width * 3)};

    int result = sws_scale(swsCtx, srcData, srcLinesize, 0, height,
                           frame->data, frame->linesize);

    if (result < 0 || result != static_cast<int>(dstHeight))
    {
        LOG_ERROR("sws_scale failed or returned wrong size: " + std::to_string(result) +
                  " (expected " + std::to_string(dstHeight) + "), src=" +
                  std::to_string(width) + "x" + std::to_string(height) +
                  ", dst=" + std::to_string(dstWidth) + "x" + std::to_string(dstHeight));
        return false;
    }

    return true;
}

bool HTTPTSStreamer::encodeVideoFrame(const uint8_t *rgbData, uint32_t width, uint32_t height, int64_t captureTimestampUs)
{
    if (!rgbData || !m_active || width == 0 || height == 0)
    {
        return false;
    }

    AVCodecContext *codecCtx = static_cast<AVCodecContext *>(m_videoCodecContext);
    AVFrame *videoFrame = static_cast<AVFrame *>(m_videoFrame);

    if (!codecCtx || !videoFrame)
    {
        return false;
    }

    // Log removido para reduzir poluição - usar [SYNC_DEBUG] logs para depuração

    // Converter RGB para YUV
    if (!convertRGBToYUV(rgbData, width, height, videoFrame))
    {
        LOG_ERROR("[VIDEO] encodeVideoFrame: convertRGBToYUV failed");
        return false;
    }

    // Configurar PTS baseado no timestamp de captura (relativo ao primeiro frame)
    // Calcular PTS em unidades do time_base do codec
    static int64_t firstVideoTimestampUs = 0;
    static bool firstFrameSet = false;

    if (!firstFrameSet)
    {
        firstVideoTimestampUs = captureTimestampUs;
        firstFrameSet = true;
    }

    // Calcular tempo relativo desde o primeiro frame (em segundos)
    int64_t relativeTimeUs = captureTimestampUs - firstVideoTimestampUs;
    double relativeTimeSeconds = static_cast<double>(relativeTimeUs) / 1000000.0;

    // Converter para unidades do time_base do codec (geralmente 1/fps)
    // codecCtx->time_base é tipicamente {1, fps} para vídeo
    AVRational timeBase = codecCtx->time_base;
    int64_t calculatedPTS = static_cast<int64_t>(relativeTimeSeconds * timeBase.den / timeBase.num);

    // Garantir que PTS do frame seja monotônico ANTES de enviar ao codec
    {
        std::lock_guard<std::mutex> lock(m_ptsMutex);
        if (m_lastVideoFramePTS >= 0 && calculatedPTS <= m_lastVideoFramePTS)
        {
            // PTS não aumentou, forçar incremento mínimo baseado no FPS
            // Incremento mínimo = 1 frame (timeBase.den / timeBase.num geralmente = fps)
            calculatedPTS = m_lastVideoFramePTS + 1;
        }
        m_lastVideoFramePTS = calculatedPTS;
    }
    videoFrame->pts = calculatedPTS;

    // Forçar primeiro frame como keyframe e keyframes periódicos
    // Usar variável de instância ao invés de static para permitir reset em reinicializações
    bool forceKeyframe = false;

    if (m_videoFrameCount == 0)
    {
        // Primeiro frame sempre é keyframe
        forceKeyframe = true;
    }
    // Forçar keyframe periódico mais frequente para permitir recuperação rápida de corrupção
    // Reduzir intervalo para metade do gop_size (a cada 1 segundo em vez de 2 segundos)
    else if (m_videoFrameCount > 0 && (m_videoFrameCount % (codecCtx->gop_size / 2) == 0))
    {
        // Keyframe periódico mais frequente para permitir recuperação de erros
        forceKeyframe = true;
    }
    // Forçar keyframe se detectamos dessincronização recente
    else if (m_desyncFrameCount > 0)
    {
        forceKeyframe = true;
        LOG_WARN("Forçando keyframe devido a dessincronização detectada (" +
                 std::to_string(m_desyncFrameCount) + " frames)");
        m_desyncFrameCount = 0; // Reset após forçar keyframe
    }

    if (forceKeyframe)
    {
        videoFrame->pict_type = AV_PICTURE_TYPE_I;
        // Usar flags do frame para garantir que o codec respeite o keyframe
        videoFrame->flags |= AV_FRAME_FLAG_KEY;
    }
    m_videoFrameCount++;

    // Enviar frame para codec
    // CRÍTICO: Tentar múltiplas vezes se codec estiver cheio para não perder frames
    int ret = avcodec_send_frame(codecCtx, videoFrame);
    if (ret < 0)
    {
        if (ret == AVERROR(EAGAIN))
        {
            // Codec está cheio, precisa receber pacotes antes de enviar mais frames
            // CRÍTICO: Receber TODOS os pacotes pendentes e tentar novamente
            // Aumentar tentativas para garantir que frames sejam enviados mesmo com codec ocupado
            const int MAX_RETRY_ATTEMPTS = 10;
            for (int retry = 0; retry < MAX_RETRY_ATTEMPTS; retry++)
            {
                AVPacket *tempPkt = av_packet_alloc();
                if (!tempPkt)
                {
                    break;
                }

                // Receber TODOS os pacotes pendentes
                while (avcodec_receive_packet(codecCtx, tempPkt) >= 0)
                {
                    // CRÍTICO: Clonar pacote ANTES de modificar qualquer coisa
                    // Modificar o pacote original pode corromper dados
                    AVPacket *pktToMux = av_packet_clone(tempPkt);
                    if (!pktToMux)
                    {
                        LOG_ERROR("encodeVideoFrame: Failed to clone packet in retry");
                        av_packet_unref(tempPkt);
                        continue;
                    }

                    // CRÍTICO: Muxar e enviar os pacotes recebidos, não descartar!
                    // Descartar pacotes faz perder frames encodados
                    AVStream *videoStream = static_cast<AVStream *>(m_videoStream);
                    if (videoStream)
                    {
                        pktToMux->stream_index = videoStream->index;
                        // Converter PTS/DTS
                        if (pktToMux->pts != AV_NOPTS_VALUE)
                        {
                            pktToMux->pts = av_rescale_q(pktToMux->pts, codecCtx->time_base, videoStream->time_base);
                        }
                        if (pktToMux->dts != AV_NOPTS_VALUE)
                        {
                            pktToMux->dts = av_rescale_q(pktToMux->dts, codecCtx->time_base, videoStream->time_base);
                        }

                        // Garantir monotonicidade PTS/DTS
                        {
                            std::lock_guard<std::mutex> lock(m_ptsMutex);
                            if (pktToMux->pts != AV_NOPTS_VALUE)
                            {
                                if (m_lastVideoPTS >= 0 && pktToMux->pts <= m_lastVideoPTS)
                                {
                                    pktToMux->pts = m_lastVideoPTS + 1;
                                }
                                m_lastVideoPTS = pktToMux->pts;
                            }
                            if (pktToMux->dts != AV_NOPTS_VALUE)
                            {
                                if (m_lastVideoDTS >= 0 && pktToMux->dts <= m_lastVideoDTS)
                                {
                                    pktToMux->dts = m_lastVideoDTS + 1;
                                }
                                m_lastVideoDTS = pktToMux->dts;
                            }
                            if (pktToMux->pts != AV_NOPTS_VALUE && pktToMux->dts != AV_NOPTS_VALUE && pktToMux->dts > pktToMux->pts)
                            {
                                pktToMux->dts = pktToMux->pts;
                                m_lastVideoDTS = pktToMux->dts;
                            }
                        }

                        // Muxar pacote clonado
                        muxPacket(pktToMux);
                        av_packet_free(&pktToMux);
                    }
                    av_packet_unref(tempPkt);
                }
                av_packet_free(&tempPkt);

                // Tentar enviar novamente
                ret = avcodec_send_frame(codecCtx, videoFrame);
                if (ret >= 0)
                {
                    break; // Sucesso!
                }
                else if (ret != AVERROR(EAGAIN))
                {
                    // Erro diferente de EAGAIN, não continuar tentando
                    break;
                }
                // Se ainda EAGAIN e não recebemos nenhum pacote, pode ser que o codec esteja realmente cheio
                // Continuar tentando
            }
        }
        if (ret < 0)
        {
            // Apenas logar se não for EAGAIN (EAGAIN é normal quando codec está cheio)
            if (ret != AVERROR(EAGAIN))
            {
                LOG_ERROR("encodeVideoFrame: avcodec_send_frame failed: " + std::to_string(ret));
            }
            // CRÍTICO: Retornar false apenas se for erro real, não EAGAIN
            // Se for EAGAIN mesmo após múltiplas tentativas, manter frame no buffer para tentar depois
            return (ret != AVERROR(EAGAIN));
        }
    }

    // Receber pacotes encodados e muxar/enviar diretamente
    AVStream *videoStream = static_cast<AVStream *>(m_videoStream);
    if (!videoStream)
    {
        LOG_ERROR("encodeVideoFrame: video stream is null");
        return false;
    }

    AVPacket *pkt = av_packet_alloc();
    int packetCount = 0;

    // CRÍTICO: Receber TODOS os pacotes disponíveis do codec
    // Não limitar tentativas - receber todos os pacotes para não perder frames
    // Isso é especialmente importante quando o codec está processando múltiplos frames
    // Remover limite de tentativas - receber até não haver mais pacotes
    while (true)
    {
        int ret = avcodec_receive_packet(codecCtx, pkt);
        if (ret < 0)
        {
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                // Codec precisa de mais frames ou terminou - parar de receber
                break;
            }
            else
            {
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("encodeVideoFrame: avcodec_receive_packet failed: " + std::string(errbuf));
                break;
            }
        }

        packetCount++;

        // CRÍTICO: Clonar pacote ANTES de modificar qualquer coisa
        // Modificar o pacote original pode corromper dados se o codec ainda estiver usando
        AVPacket *pktToMux = av_packet_clone(pkt);
        if (!pktToMux)
        {
            LOG_ERROR("encodeVideoFrame: Failed to clone packet");
            av_packet_unref(pkt);
            continue; // Pular este pacote e tentar o próximo
        }

        // Configurar stream_index do pacote
        pktToMux->stream_index = videoStream->index;

        // Converter PTS/DTS do time_base do codec para o time_base do stream
        if (pktToMux->pts != AV_NOPTS_VALUE)
        {
            pktToMux->pts = av_rescale_q(pktToMux->pts, codecCtx->time_base, videoStream->time_base);
        }
        if (pktToMux->dts != AV_NOPTS_VALUE)
        {
            pktToMux->dts = av_rescale_q(pktToMux->dts, codecCtx->time_base, videoStream->time_base);
        }

        // Garantir que PTS e DTS sejam monotônicos (sempre aumentem)
        // Detectar dessincronização baseada em saltos grandes de PTS/DTS
        {
            std::lock_guard<std::mutex> lock(m_ptsMutex);
            bool desyncDetected = false;

            if (pktToMux->pts != AV_NOPTS_VALUE)
            {
                if (m_lastVideoPTS >= 0)
                {
                    // Calcular incremento esperado baseado no FPS
                    // Para 60 FPS, cada frame deve ter PTS incrementado por ~1/60 do time_base
                    int64_t expectedIncrement = videoStream->time_base.den / static_cast<int>(m_fps);
                    int64_t actualIncrement = pktToMux->pts - m_lastVideoPTS;

                    // Detectar salto muito grande (mais de 2x o esperado) ou regressão
                    if (pktToMux->pts <= m_lastVideoPTS || actualIncrement > expectedIncrement * 2)
                    {
                        desyncDetected = true;
                        m_desyncFrameCount++;
                    }
                    else
                    {
                        m_desyncFrameCount = 0; // Reset contador se tudo está OK
                    }

                    if (pktToMux->pts <= m_lastVideoPTS)
                    {
                        // PTS não aumentou, forçar incremento mínimo
                        pktToMux->pts = m_lastVideoPTS + 1;
                    }
                }
                m_lastVideoPTS = pktToMux->pts;
            }
            if (pktToMux->dts != AV_NOPTS_VALUE)
            {
                if (m_lastVideoDTS >= 0 && pktToMux->dts <= m_lastVideoDTS)
                {
                    // DTS não aumentou, forçar incremento mínimo
                    pktToMux->dts = m_lastVideoDTS + 1;
                    desyncDetected = true;
                }
                m_lastVideoDTS = pktToMux->dts;
            }
            // Garantir DTS <= PTS (requisito do MPEG-TS)
            if (pktToMux->pts != AV_NOPTS_VALUE && pktToMux->dts != AV_NOPTS_VALUE && pktToMux->dts > pktToMux->pts)
            {
                pktToMux->dts = pktToMux->pts;
                m_lastVideoDTS = pktToMux->dts;
                desyncDetected = true;
            }

            // Se detectamos dessincronização múltiplas vezes consecutivas, forçar keyframe
            if (desyncDetected && m_desyncFrameCount >= 3)
            {
                // Forçar próximo frame como keyframe para permitir recuperação
                // Isso será aplicado no próximo encodeVideoFrame
                LOG_WARN("Desincronização detectada (" + std::to_string(m_desyncFrameCount) +
                         " frames), próximo frame será keyframe para recuperação");
                m_desyncFrameCount = 0; // Reset após detectar
            }
        }

        // Logs removidos para melhorar performance - logs são um gargalo significativo

        // Muxar e enviar pacote (muxPacket ainda fará outra cópia por segurança)
        if (!muxPacket(pktToMux))
        {
            LOG_ERROR("encodeVideoFrame: muxPacket failed");
        }

        // Liberar cópia do pacote (muxPacket já fez sua própria cópia)
        av_packet_free(&pktToMux);

        // Liberar pacote original do codec
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    // Após o primeiro keyframe ser enviado, fazer flush explícito para acelerar início do playback
    if (m_videoFrameCount == 1 && packetCount > 0)
    {
        // Após o primeiro keyframe ser enviado, fazer flush explícito para acelerar início do playback
        AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_muxerContext);
        if (formatCtx)
        {
            // Usar o mesmo mutex usado em muxPacket para proteger o format context
            // Não há mutex específico, então vamos usar o lock do muxPacket se necessário
            // Por enquanto, vamos apenas fazer o flush sem lock adicional (muxPacket já protege)
            av_write_frame(formatCtx, nullptr); // Flush explícito
        }
    }
    // Log removido para reduzir poluição - usar [SYNC_DEBUG] logs para depuração

    return true;
}

bool HTTPTSStreamer::convertInt16ToFloatPlanar(const int16_t *samples, size_t sampleCount, void *audioFrame, size_t outputSamples)
{
    if (!samples || !audioFrame || sampleCount == 0 || outputSamples == 0)
    {
        return false;
    }

    SwrContext *swrCtx = static_cast<SwrContext *>(m_swrContext);
    AVFrame *frame = static_cast<AVFrame *>(audioFrame);

    if (!swrCtx || !frame)
    {
        return false;
    }

    if (av_frame_make_writable(frame) < 0)
    {
        return false;
    }

    const uint8_t *srcData[1] = {reinterpret_cast<const uint8_t *>(samples)};
    const int inputSamples = static_cast<int>(sampleCount / m_audioChannelsCount);

    int ret = swr_convert(swrCtx, frame->data, static_cast<int>(outputSamples),
                          srcData, inputSamples);
    if (ret < 0)
    {
        LOG_ERROR("swr_convert failed: " + std::to_string(ret));
        return false;
    }

    if (ret != static_cast<int>(outputSamples))
    {
        LOG_WARN("swr_convert returned " + std::to_string(ret) + " samples, expected " + std::to_string(outputSamples));
    }

    frame->nb_samples = static_cast<int>(outputSamples);
    return true;
}

bool HTTPTSStreamer::encodeAudioFrame(const int16_t *samples, size_t sampleCount, int64_t captureTimestampUs)
{
    if (!samples || !m_active || sampleCount == 0)
    {
        return false;
    }

    AVCodecContext *codecCtx = static_cast<AVCodecContext *>(m_audioCodecContext);
    AVFrame *audioFrame = static_cast<AVFrame *>(m_audioFrame);

    if (!codecCtx || !audioFrame)
    {
        LOG_ERROR("encodeAudioFrame: codec context or audio frame is null");
        return false;
    }

    // Acumular samples até ter o suficiente para um frame completo
    {
        std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
        m_audioAccumulator.insert(m_audioAccumulator.end(), samples, samples + sampleCount);
    }

    const int samplesPerFrame = codecCtx->frame_size;
    if (samplesPerFrame <= 0)
    {
        LOG_ERROR("encodeAudioFrame: invalid frame_size: " + std::to_string(samplesPerFrame));
        return false;
    }

    const int totalSamplesNeeded = samplesPerFrame * m_audioChannelsCount;

    // Processar frames completos enquanto tivermos samples suficientes
    AVStream *audioStream = static_cast<AVStream *>(m_audioStream);
    if (!audioStream)
    {
        LOG_ERROR("encodeAudioFrame: audio stream is null");
        return false;
    }

    static int64_t firstAudioTimestampUs = 0;
    static bool firstAudioFrameSet = false;
    static int64_t frameCount = 0;

    if (!firstAudioFrameSet)
    {
        firstAudioTimestampUs = captureTimestampUs;
        firstAudioFrameSet = true;
    }

    bool hadError = false;

    while (true)
    {
        std::vector<int16_t> frameSamples;
        {
            std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
            if (m_audioAccumulator.size() < static_cast<size_t>(totalSamplesNeeded))
            {
                break; // Não temos samples suficientes - isso é normal, não é erro
            }

            // Pegar samples para um frame completo
            frameSamples.assign(m_audioAccumulator.begin(),
                                m_audioAccumulator.begin() + totalSamplesNeeded);
            m_audioAccumulator.erase(m_audioAccumulator.begin(),
                                     m_audioAccumulator.begin() + totalSamplesNeeded);
        }

        // Converter int16 para float planar
        if (!convertInt16ToFloatPlanar(frameSamples.data(), frameSamples.size(),
                                       audioFrame, samplesPerFrame))
        {
            LOG_ERROR("encodeAudioFrame: convertInt16ToFloatPlanar failed");
            hadError = true;
            break;
        }

        // Configurar PTS baseado no timestamp de captura (relativo ao primeiro chunk)
        // Calcular tempo relativo desde o primeiro chunk (em segundos)
        int64_t relativeTimeUs = captureTimestampUs - firstAudioTimestampUs;
        double relativeTimeSeconds = static_cast<double>(relativeTimeUs) / 1000000.0;

        // Converter para unidades do time_base do codec (geralmente 1/sampleRate para áudio)
        AVRational timeBase = codecCtx->time_base;
        int64_t calculatedPTS = static_cast<int64_t>(relativeTimeSeconds * timeBase.den / timeBase.num);

        // Garantir que PTS do frame seja monotônico ANTES de enviar ao codec
        {
            std::lock_guard<std::mutex> lock(m_ptsMutex);
            if (m_lastAudioFramePTS >= 0 && calculatedPTS <= m_lastAudioFramePTS)
            {
                // PTS não aumentou, forçar incremento mínimo baseado no frame_size
                // Incremento mínimo = 1 frame de áudio (samplesPerFrame)
                calculatedPTS = m_lastAudioFramePTS + samplesPerFrame;
            }
            m_lastAudioFramePTS = calculatedPTS;
        }
        audioFrame->pts = calculatedPTS;

        // Timestamp removido - não é mais necessário sem logs
        frameCount++;

        // Enviar frame para codec
        int ret = avcodec_send_frame(codecCtx, audioFrame);
        if (ret < 0)
        {
            if (ret == AVERROR(EAGAIN))
            {
                // Codec precisa receber mais pacotes antes de aceitar novos frames
                // Isso é normal, não é erro
                break;
            }
            else
            {
                LOG_ERROR("encodeAudioFrame: avcodec_send_frame failed: " + std::to_string(ret));
                hadError = true;
                break;
            }
        }

        // Receber pacotes encodados e muxar/enviar diretamente
        AVPacket *pkt = av_packet_alloc();
        int packetCount = 0;
        while (avcodec_receive_packet(codecCtx, pkt) >= 0)
        {
            packetCount++;

            // CRÍTICO: Fazer cópia do pacote ANTES de modificar qualquer coisa
            // Isso evita corromper os dados originais do codec
            AVPacket *pktCopy = av_packet_clone(pkt);
            if (!pktCopy)
            {
                LOG_ERROR("encodeAudioFrame: Failed to clone packet");
                av_packet_unref(pkt);
                continue; // Pular este pacote se não conseguirmos clonar
            }

            // Configurar stream_index do pacote (na cópia)
            pktCopy->stream_index = audioStream->index;

            // Converter PTS/DTS do time_base do codec para o time_base do stream (na cópia)
            if (pktCopy->pts != AV_NOPTS_VALUE)
            {
                pktCopy->pts = av_rescale_q(pktCopy->pts, codecCtx->time_base, audioStream->time_base);
            }
            if (pktCopy->dts != AV_NOPTS_VALUE)
            {
                pktCopy->dts = av_rescale_q(pktCopy->dts, codecCtx->time_base, audioStream->time_base);
            }

            // Garantir que PTS e DTS sejam monotônicos (sempre aumentem) - na cópia
            {
                std::lock_guard<std::mutex> lock(m_ptsMutex);
                if (pktCopy->pts != AV_NOPTS_VALUE)
                {
                    if (m_lastAudioPTS >= 0 && pktCopy->pts <= m_lastAudioPTS)
                    {
                        // PTS não aumentou, forçar incremento mínimo
                        pktCopy->pts = m_lastAudioPTS + 1;
                    }
                    m_lastAudioPTS = pktCopy->pts;
                }
                if (pktCopy->dts != AV_NOPTS_VALUE)
                {
                    if (m_lastAudioDTS >= 0 && pktCopy->dts <= m_lastAudioDTS)
                    {
                        // DTS não aumentou, forçar incremento mínimo
                        pktCopy->dts = m_lastAudioDTS + 1;
                    }
                    m_lastAudioDTS = pktCopy->dts;
                }
                // Garantir DTS <= PTS (requisito do MPEG-TS)
                if (pktCopy->pts != AV_NOPTS_VALUE && pktCopy->dts != AV_NOPTS_VALUE && pktCopy->dts > pktCopy->pts)
                {
                    pktCopy->dts = pktCopy->pts;
                    m_lastAudioDTS = pktCopy->dts;
                }
            }

            // Logs removidos para melhorar performance - logs são um gargalo significativo

            // Muxar e enviar pacote (usar cópia, não o original)
            // muxPacket não precisa fazer outra cópia agora, mas vamos manter por segurança
            if (!muxPacket(pktCopy))
            {
                LOG_ERROR("encodeAudioFrame: muxPacket failed");
            }

            // Liberar cópia e original
            av_packet_free(&pktCopy);
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);

        if (packetCount > 0)
        {
        }
    }

    // Retornar false apenas se houve erro real, não se apenas não havia samples suficientes
    if (hadError)
    {
        return false;
    }

    // Se não encodamos nada mas não houve erro, retornar true (é normal não ter samples suficientes ainda)
    return true;
}

bool HTTPTSStreamer::muxPacket(void *packet)
{
    if (!packet || m_stopRequest)
    {
        return false;
    }

    // Verificar se m_muxerContext ainda é válido (pode ter sido invalidado durante stop)
    void *muxerCtx = m_muxerContext;
    if (!muxerCtx)
    {
        return false;
    }

    AVFormatContext *formatCtx = static_cast<AVFormatContext *>(muxerCtx);
    if (!formatCtx || !formatCtx->pb)
    {
        return false;
    }

    AVPacket *pkt = static_cast<AVPacket *>(packet);

    // NOTA: O pacote já foi clonado antes de chamar esta função (em encodeVideoFrame/encodeAudioFrame)
    // Não precisamos clonar novamente aqui - o pacote já está seguro e o muxing é protegido por mutex
    // A clonagem dupla pode causar problemas e é desnecessária
    AVPacket *pktCopy = pkt;

    // Garantir que DTS seja válido e monotônico
    if (pktCopy->dts == AV_NOPTS_VALUE)
    {
        // Se DTS não está definido, usar PTS
        if (pktCopy->pts != AV_NOPTS_VALUE)
        {
            pktCopy->dts = pktCopy->pts;
        }
        else
        {
            LOG_ERROR("muxPacket: Both PTS and DTS are invalid");
            // Não liberar pktCopy aqui pois não foi alocado por nós (é apenas um ponteiro para pkt)
            return false;
        }
    }

    // Garantir que DTS <= PTS (requisito do MPEG-TS)
    if (pktCopy->pts != AV_NOPTS_VALUE && pktCopy->dts > pktCopy->pts)
    {
        pktCopy->dts = pktCopy->pts;
    }

    // Proteger av_interleaved_write_frame com mutex (não é thread-safe)
    // O writeCallback também será chamado dentro deste lock, garantindo thread-safety
    {
        std::lock_guard<std::mutex> lock(m_muxMutex);

        // Verificar novamente após adquirir o lock (pode ter mudado durante a espera)
        if (m_stopRequest || !m_muxerContext || !formatCtx || !formatCtx->pb)
        {
            return false;
        }

        int ret = av_interleaved_write_frame(formatCtx, pktCopy);
        if (ret < 0)
        {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Failed to write packet (stream=" + std::to_string(pktCopy->stream_index) +
                      ", pts=" + std::to_string(pktCopy->pts) + ", dts=" + std::to_string(pktCopy->dts) +
                      "): " + std::string(errbuf));
            // Não liberar pktCopy aqui pois não foi alocado por nós
            return false;
        }
    }

    // Não liberar o pacote aqui - ele será liberado pela função chamadora
    // (encodeVideoFrame ou encodeAudioFrame já fazem isso)
    return true;
}

int64_t HTTPTSStreamer::getTimestampUs() const
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000LL + static_cast<int64_t>(ts.tv_nsec) / 1000LL;
}

void HTTPTSStreamer::cleanupOldData()
{
    // Limpar dados antigos baseado em tempo (não em quantidade)
    // Remover apenas dados que estão fora da janela temporal máxima
    // OTIMIZAÇÃO: Se detectamos dessincronização, ser mais agressivo em descartar frames antigos
    // para permitir recuperação mais rápida pulando para o tempo atual

    // Calcular timestamp mais antigo permitido (janela temporal)
    // Reduzir janela se há dessincronização para permitir recuperação mais rápida
    int64_t bufferTimeUs = MAX_BUFFER_TIME_US;
    if (m_desyncFrameCount > 0)
    {
        // Reduzir janela pela metade quando há dessincronização
        bufferTimeUs = MAX_BUFFER_TIME_US / 2;
    }

    int64_t oldestAllowedVideoUs = m_latestVideoTimestampUs - bufferTimeUs;
    int64_t oldestAllowedAudioUs = m_latestAudioTimestampUs - bufferTimeUs;

    // CRÍTICO: NÃO descartar frames não processados normalmente - isso causa perda de frames!
    // Remover apenas frames processados que estão fora da janela temporal
    // EXCEÇÃO: Se há dessincronização, descartar frames não processados muito antigos
    // para permitir recuperação mais rápida pulando para o tempo atual
    while (!m_timestampedVideoBuffer.empty())
    {
        const auto &frame = m_timestampedVideoBuffer.front();

        // Se está dentro da janela temporal, manter (mesmo que processado)
        if (frame.captureTimestampUs >= oldestAllowedVideoUs)
        {
            break;
        }

        // Se há dessincronização, descartar frames não processados muito antigos
        // para permitir recuperação mais rápida pulando para o tempo atual
        if (m_desyncFrameCount > 0 && !frame.processed)
        {
            LOG_WARN("Descartando frame não processado antigo devido a dessincronização (timestamp: " +
                     std::to_string(frame.captureTimestampUs) + " us, atual: " +
                     std::to_string(m_latestVideoTimestampUs) + " us)");
            m_timestampedVideoBuffer.pop_front();
            continue;
        }

        // CRÍTICO: NÃO descartar frames não processados normalmente - eles ainda precisam ser encodados!
        // Apenas descartar frames que já foram processados E estão muito antigos
        if (frame.processed)
        {
            // Frame processado e antigo - seguro descartar
            m_timestampedVideoBuffer.pop_front();
        }
        else
        {
            // Frame não processado - MANTER mesmo que antigo!
            // Isso pode acontecer se o encoding está lento, mas não devemos perder frames
            // Continuar processando frames antigos até que sejam encodados
            break;
        }
    }

    // Mesma lógica para áudio
    while (!m_timestampedAudioBuffer.empty())
    {
        const auto &audio = m_timestampedAudioBuffer.front();

        if (audio.captureTimestampUs >= oldestAllowedAudioUs)
        {
            break;
        }

        m_timestampedAudioBuffer.pop_front();
    }
}

HTTPTSStreamer::SyncZone HTTPTSStreamer::calculateSyncZone()
{
    std::lock_guard<std::mutex> videoLock(m_videoBufferMutex);
    std::lock_guard<std::mutex> audioLock(m_audioBufferMutex);

    if (m_timestampedVideoBuffer.empty() || m_timestampedAudioBuffer.empty())
    {
        return SyncZone::invalid();
    }

    // Encontrar sobreposição temporal entre buffers
    int64_t videoStartUs = m_timestampedVideoBuffer.front().captureTimestampUs;
    int64_t videoEndUs = m_timestampedVideoBuffer.back().captureTimestampUs;

    int64_t audioStartUs = m_timestampedAudioBuffer.front().captureTimestampUs;
    int64_t audioEndUs = m_timestampedAudioBuffer.back().captureTimestampUs;

    // Calcular sobreposição
    int64_t overlapStartUs = std::max(videoStartUs, audioStartUs);
    int64_t overlapEndUs = std::min(videoEndUs, audioEndUs);

    // Verificar se há sobreposição (qualquer sobreposição é suficiente para começar a processar)
    if (overlapEndUs <= overlapStartUs)
    {
        return SyncZone::invalid(); // Não há sobreposição temporal
    }

    // Zona sincronizada: processar desde o início da sobreposição até o final
    // Para 60fps, precisamos processar imediatamente, não esperar por uma zona "segura"
    int64_t zoneStartUs = overlapStartUs;
    int64_t zoneEndUs = overlapEndUs;

    // Encontrar índices correspondentes nos buffers
    size_t videoStartIdx = 0;
    size_t videoEndIdx = 0;
    for (size_t i = 0; i < m_timestampedVideoBuffer.size(); i++)
    {
        if (m_timestampedVideoBuffer[i].captureTimestampUs >= zoneStartUs && videoStartIdx == 0)
        {
            videoStartIdx = i;
        }
        if (m_timestampedVideoBuffer[i].captureTimestampUs <= zoneEndUs)
        {
            videoEndIdx = i + 1;
        }
    }

    size_t audioStartIdx = 0;
    size_t audioEndIdx = 0;
    for (size_t i = 0; i < m_timestampedAudioBuffer.size(); i++)
    {
        if (m_timestampedAudioBuffer[i].captureTimestampUs >= zoneStartUs && audioStartIdx == 0)
        {
            audioStartIdx = i;
        }
        if (m_timestampedAudioBuffer[i].captureTimestampUs <= zoneEndUs)
        {
            audioEndIdx = i + 1;
        }
    }

    SyncZone zone;
    zone.startTimeUs = zoneStartUs;
    zone.endTimeUs = zoneEndUs;
    zone.videoStartIdx = videoStartIdx;
    zone.videoEndIdx = videoEndIdx;
    zone.audioStartIdx = audioStartIdx;
    zone.audioEndIdx = audioEndIdx;

    return zone;
}

void HTTPTSStreamer::serverThread()
{
    std::string protocol = m_httpServer.isHTTPS() ? "HTTPS" : "HTTP";
    LOG_INFO("Server thread started, waiting for " + protocol + " connections on port " + std::to_string(m_port));

    while (m_running)
    {
        if (m_stopRequest)
        {
            break;
        }

        // Aceitar conexão de cliente usando HTTPServer
        int clientFd = m_httpServer.acceptClient();
        if (clientFd < 0)
        {
            if (m_running && !m_stopRequest)
            {
                // Erro ao aceitar (pode ser porque fechamos o servidor)
                // Não logar erro se foi fechado intencionalmente
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            break; // Sair do loop se não estiver rodando ou se o servidor foi fechado
        }

        // Processar cliente em thread separada
        std::thread clientThread(&HTTPTSStreamer::handleClient, this, clientFd);
        clientThread.detach(); // Detach para não precisar fazer join
    }

    LOG_INFO("Server thread stopped");

    return;
}

void HTTPTSStreamer::encodingThread()
{
    // Aguardar um pouco antes de começar para evitar processar frames muito antigos
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    while (m_running)
    {
        if (m_stopRequest)
        {
            break;
        }

        bool processedAny = false;

        // Processar frames de vídeo disponíveis (sem esperar por sincronização)
        // OTIMIZAÇÃO CRÍTICA: Copiar dados para fora do lock e fazer encoding sem lock
        // Isso evita bloquear outras threads durante o encoding (que pode ser lento)
        std::vector<std::tuple<std::shared_ptr<std::vector<uint8_t>>, uint32_t, uint32_t, int64_t, size_t>> framesToProcess;
        size_t totalFrames = 0;

        {
            std::lock_guard<std::mutex> videoLock(m_videoBufferMutex);
            totalFrames = m_timestampedVideoBuffer.size();

            // CRÍTICO: Se há dessincronização ou muitos frames acumulados, pular frames antigos
            // para permitir recuperação mais rápida
            const size_t MAX_BUFFER_FRAMES = static_cast<size_t>(m_fps * 2); // Máximo 2 segundos de frames
            bool shouldSkipOldFrames = false;

            // Verificar se há dessincronização ou buffer muito grande
            {
                std::lock_guard<std::mutex> ptsLock(m_ptsMutex);
                if (m_desyncFrameCount > 0 || totalFrames > MAX_BUFFER_FRAMES)
                {
                    shouldSkipOldFrames = true;
                }
            }

            if (shouldSkipOldFrames && totalFrames > 10)
            {
                // Pular frames antigos não processados - manter apenas os mais recentes
                // Calcular quantos frames manter (últimos 0.5 segundos para recuperação mais rápida)
                size_t framesToKeep = static_cast<size_t>(m_fps / 2); // Apenas 0.5 segundo
                if (totalFrames > framesToKeep)
                {
                    size_t framesToSkip = totalFrames - framesToKeep;
                    int64_t currentTimeUs = getTimestampUs();

                    LOG_WARN("Pulando " + std::to_string(framesToSkip) + " frames antigos devido a " +
                             (m_desyncFrameCount > 0 ? "dessincronização" : "buffer grande") +
                             " (total: " + std::to_string(totalFrames) + " frames, mantendo últimos " +
                             std::to_string(framesToKeep) + " frames)");

                    // Remover frames antigos não processados
                    size_t skipped = 0;
                    while (!m_timestampedVideoBuffer.empty() && skipped < framesToSkip)
                    {
                        auto &frame = m_timestampedVideoBuffer.front();
                        // Pular apenas frames não processados que estão muito atrás no tempo
                        // Ser mais agressivo: pular se está mais de 0.5 segundos atrás
                        if (!frame.processed &&
                            (currentTimeUs - frame.captureTimestampUs) > 500000) // Mais de 0.5 segundo atrás
                        {
                            m_timestampedVideoBuffer.pop_front();
                            skipped++;
                        }
                        else
                        {
                            // Se chegamos em frames processados ou recentes, parar
                            break;
                        }
                    }

                    if (skipped > 0)
                    {
                        LOG_INFO("Pulados " + std::to_string(skipped) + " frames antigos, buffer agora tem " +
                                 std::to_string(m_timestampedVideoBuffer.size()) + " frames");
                        // Resetar contador de dessincronização após pular frames
                        {
                            std::lock_guard<std::mutex> ptsLock(m_ptsMutex);
                            m_desyncFrameCount = 0;
                        }
                    }

                    // Atualizar totalFrames após pular
                    totalFrames = m_timestampedVideoBuffer.size();
                }
            }

            // CRÍTICO: Processar TODOS os frames disponíveis, não limitar!
            // Limitar frames causa acúmulo e perda de frames
            // Para 60 FPS, precisamos processar todos os frames o mais rápido possível
            const size_t MAX_FRAMES_PER_ITERATION = totalFrames; // Processar TODOS os frames disponíveis

            // Copiar dados dos frames não processados para fora do lock
            size_t framesCopied = 0;
            for (size_t i = 0; i < m_timestampedVideoBuffer.size() && framesCopied < MAX_FRAMES_PER_ITERATION; i++)
            {
                auto &frame = m_timestampedVideoBuffer[i];
                if (!frame.processed && frame.data && frame.width > 0 && frame.height > 0)
                {
                    // Copiar referência aos dados (shared_ptr já gerencia memória)
                    framesToProcess.push_back({frame.data, frame.width, frame.height, frame.captureTimestampUs, i});
                    framesCopied++;
                }
            }
        }

        // Processar frames FORA do lock (encoding pode ser lento)
        // OTIMIZAÇÃO: Batch dinâmico baseado no tamanho da fila para melhor throughput
        // Se há muitos frames acumulados, processar mais agressivamente
        std::vector<size_t> processedIndices;
        size_t MAX_FRAMES_PER_BATCH;
        if (totalFrames > 50)
        {
            MAX_FRAMES_PER_BATCH = 20; // Muitos frames acumulados - processar mais agressivamente
        }
        else if (totalFrames > 20)
        {
            MAX_FRAMES_PER_BATCH = 15; // Fila média - processar moderadamente
        }
        else
        {
            MAX_FRAMES_PER_BATCH = 10; // Fila pequena - processar normalmente
        }
        size_t framesProcessed = 0;

        for (const auto &[data, width, height, timestamp, bufferIndex] : framesToProcess)
        {
            // Verificar se parada foi solicitada
            if (m_stopRequest)
            {
                break;
            }

            // Limitar frames por batch para evitar que o codec fique cheio
            if (framesProcessed >= MAX_FRAMES_PER_BATCH)
            {
                break; // Processar o resto na próxima iteração
            }

            // Tentar encodar o frame (sem lock)
            bool encodeSuccess = encodeVideoFrame(data->data(), width, height, timestamp);

            // Guardar índice se sucesso (marcar depois em batch)
            if (encodeSuccess)
            {
                processedIndices.push_back(bufferIndex);
                processedAny = true;
                framesProcessed++;
            }
            else
            {
                // Se falhou (provavelmente EAGAIN - codec cheio), parar e tentar novamente na próxima iteração
                // Isso evita que muitos frames falhem de uma vez
                break;
            }
        }

        // Marcar todos os frames processados de uma vez (lock único)
        if (!processedIndices.empty())
        {
            std::lock_guard<std::mutex> videoLock(m_videoBufferMutex);
            for (size_t idx : processedIndices)
            {
                if (idx < m_timestampedVideoBuffer.size())
                {
                    auto &frame = m_timestampedVideoBuffer[idx];
                    if (!frame.processed)
                    {
                        frame.processed = true;
                    }
                }
            }
        }

        // OTIMIZAÇÃO: Remover frames processados em batch (mais eficiente que pop_front individual)
        // IMPORTANTE: Só remover se houver muitos frames processados para evitar remover frames importantes
        {
            std::lock_guard<std::mutex> videoLock(m_videoBufferMutex);
            // Contar quantos frames processados existem no início
            size_t processedCount = 0;
            while (processedCount < m_timestampedVideoBuffer.size() &&
                   m_timestampedVideoBuffer[processedCount].processed)
            {
                processedCount++;
            }
            // Remover todos de uma vez, mas apenas se houver pelo menos 5 frames processados
            // Isso evita remover frames muito cedo que podem ser necessários para sincronização
            if (processedCount >= 5)
            {
                m_timestampedVideoBuffer.erase(m_timestampedVideoBuffer.begin(),
                                               m_timestampedVideoBuffer.begin() + processedCount);
            }
        }

        // Processar chunks de áudio disponíveis (sem esperar por sincronização)
        // IMPORTANTE: Só marcar como processado se o encoding foi bem-sucedido
        {
            std::lock_guard<std::mutex> audioLock(m_audioBufferMutex);

            // Processar chunks não processados
            for (auto &audio : m_timestampedAudioBuffer)
            {
                // Verificar se parada foi solicitada
                if (m_stopRequest)
                {
                    break;
                }

                if (!audio.processed && audio.samples && audio.sampleCount > 0)
                {
                    // Tentar encodar o chunk
                    bool encodeSuccess = encodeAudioFrame(audio.samples->data(), audio.sampleCount, audio.captureTimestampUs);

                    // SÓ marcar como processado se o encoding foi bem-sucedido
                    if (encodeSuccess)
                    {
                        audio.processed = true; // Marcar como processado apenas se sucesso
                        processedAny = true;
                    }
                    else
                    {
                        // Encoding falhou - NÃO marcar como processado, manter no buffer
                        // Áudio geralmente não falha, mas manter consistência
                    }
                }
            }

            // Remover chunks processados do início
            while (!m_timestampedAudioBuffer.empty() && m_timestampedAudioBuffer.front().processed)
            {
                m_timestampedAudioBuffer.pop_front();
            }
        }

        // Se processamos algo, continuar imediatamente (sem sleep) para máxima velocidade
        // Se não processamos nada, verificar se há dados pendentes antes de dormir
        if (!processedAny)
        {
            // Verificar se há frames ou áudio pendentes antes de dormir
            bool hasPendingData = false;
            {
                std::lock_guard<std::mutex> videoLock(m_videoBufferMutex);
                hasPendingData = !m_timestampedVideoBuffer.empty();
            }
            if (!hasPendingData)
            {
                std::lock_guard<std::mutex> audioLock(m_audioBufferMutex);
                hasPendingData = !m_timestampedAudioBuffer.empty();
            }

            if (!hasPendingData)
            {
                // Só dormir se realmente não há dados para processar
                std::this_thread::sleep_for(std::chrono::microseconds(1)); // 1µs - mínimo possível
            }
            // Se há dados pendentes mas não processamos, continuar imediatamente (pode ser codec ocupado)
            // Não fazer sleep - tentar processar novamente imediatamente
        }
        // Se processamos, não dormir - continuar imediatamente para processar mais frames e alcançar 60 FPS
    }

    LOG_INFO("Encoding thread stopped");

    return;
}
