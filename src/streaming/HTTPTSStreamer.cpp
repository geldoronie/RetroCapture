#include "HTTPTSStreamer.h"
#include "../utils/HttpAuth.h"
#include "../utils/Logger.h"
#include "../utils/Paths.h"

#ifdef PLATFORM_LINUX
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <time.h>
#include <fstream>
#include <sstream>
#include "../utils/FilesystemCompat.h"

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
// Diferentes versões do FFmpeg têm assinaturas diferentes:
// - FFmpeg 6.1+ (libavformat 61+): const uint8_t*
// - FFmpeg 6.0- (libavformat 60-): uint8_t* (não const)
// Usamos const uint8_t* (mais seguro) e fazemos cast quando necessário
static int writeCallback(void *opaque, const uint8_t *buf, int buf_size)
{
    HTTPTSStreamer *streamer = static_cast<HTTPTSStreamer *>(opaque);
    return streamer->writeToClients(buf, buf_size);
}

// Wrapper para compatibilidade com versões do FFmpeg que esperam uint8_t* (não const)
// FFmpeg 6.0 (libavformat 60) ainda usa uint8_t* (não const)
__attribute__((unused)) static int writeCallbackNonConst(void *opaque, uint8_t *buf, int buf_size)
{
    return writeCallback(opaque, const_cast<const uint8_t*>(buf), buf_size);
}

// Incluir FFmpegCompat.h após definir callbacks para ter acesso às macros
#include "../utils/FFmpegCompat.h"

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

    return true;
}

void HTTPTSStreamer::setAudioFormat(uint32_t sampleRate, uint32_t channels)
{
    m_audioSampleRate = sampleRate;
    m_audioChannelsCount = channels;
}

void HTTPTSStreamer::enableWebPortal(bool enable)
{
    m_webPortalEnabled = enable;
    // Se Web Portal for desabilitado, também desabilitar HTTPS
    if (!enable && m_enableHTTPS)
    {
        m_enableHTTPS = false;
    }
}

void HTTPTSStreamer::setVideoCodec(const std::string &codecName)
{
    m_videoCodecName = codecName;
}

void HTTPTSStreamer::setBufferConfig(size_t maxVideoBufferSize, size_t maxAudioBufferSize,
                                     int64_t maxBufferTimeSeconds,
                                     size_t avioBufferSize)
{
    m_maxVideoBufferSize = maxVideoBufferSize;
    m_maxAudioBufferSize = maxAudioBufferSize;
    m_maxBufferTimeSeconds = maxBufferTimeSeconds;
    m_avioBufferSize = avioBufferSize;
}

void HTTPTSStreamer::setAudioCodec(const std::string &codecName)
{
    m_audioCodecName = codecName;
}

void HTTPTSStreamer::setStreamPasswordHash(const std::string &sha256Hex)
{
    {
        std::lock_guard<std::mutex> lock(m_passwordMu);
        m_streamPasswordHash = sha256Hex;
    }
    // Forward to the embedded APIController so /meta (which is routed
    // through it, not handled here) shares the same gate.
    m_apiController.setStreamPasswordHash(sha256Hex);
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

    // Adicionar frame ao MediaSynchronizer
    return m_streamSynchronizer.addVideoFrame(data, width, height, captureTimestampUs);
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

    // Adicionar áudio ao MediaSynchronizer
    bool ok = m_streamSynchronizer.addAudioChunk(samples, sampleCount, captureTimestampUs, m_audioSampleRate, m_audioChannelsCount);

    // Phase 2 of #47: same audio chunk feeds the /raw pipeline. The /raw
    // muxer needs audio packets too — only the video frame source differs
    // between /stream (post-shader) and /raw (pre-shader).
    //
    // Gate on hasRawClients() so the /raw audio encoder doesn't start
    // running its PTS clock before any /raw video frame is pushed. Without
    // this gate, audio would accumulate its sample-count-based PTS for the
    // full pre-connect window while video stays at PTS=0; the muxer then
    // emits audio packets at "stream has been running for N seconds" while
    // video starts at 0, which ffplay flags as "DTS … < … out of order"
    // and our client renders as the visible ghost / back-and-forth jitter.
    // With the gate, both streams begin their PTS counters at the same
    // wall-clock moment — the first frame after the first client arrives.
    if (m_rawMediaEncoder.isInitialized() && hasRawClients())
    {
        m_rawStreamSynchronizer.addAudioChunk(samples, sampleCount, captureTimestampUs, m_audioSampleRate, m_audioChannelsCount);
    }

    return ok;
}

bool HTTPTSStreamer::pushRawFrame(const uint8_t *data, uint32_t width, uint32_t height)
{
    if (!data || !m_active || width == 0 || height == 0)
    {
        return false;
    }
    if (!m_rawMediaEncoder.isInitialized())
    {
        // Raw pipeline didn't come up — accept the push as a no-op so callers
        // don't have to special-case it.
        return false;
    }
    int64_t captureTimestampUs = getTimestampUs();
    return m_rawStreamSynchronizer.addVideoFrame(data, width, height, captureTimestampUs);
}

bool HTTPTSStreamer::start()
{
    if (m_active)
    {
        return true; // Já está ativo
    }

    // Verificar se ainda está em cooldown após parar
    if (!canStart())
    {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_stopTime).count();
        auto remaining = STOP_COOLDOWN_MS - elapsed;
        LOG_WARN("Streaming ainda em cooldown. Aguarde " + std::to_string(remaining / 1000) + " segundos");
        return false;
    }

    // Resetar stopTime quando iniciar com sucesso
    {
        std::lock_guard<std::mutex> lock(m_stopTimeMutex);
        m_stopTime = std::chrono::steady_clock::time_point();
    }

    // Fechar servidor anterior se existir
    m_httpServer.closeServer();
    // Aguardar um pouco para o SO liberar a porta (reduzido ao mínimo para evitar bloqueio)
    // Esta operação já está em thread separada, mas ainda pode bloquear brevemente
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Limpar recursos de encoding se existirem (pode ter sido parado anteriormente)
    if (m_mediaEncoder.isInitialized() || m_mediaMuxer.isInitialized())
    {
        cleanupEncoding();
    }

    // Inicializar encoding primeiro
    if (!initializeEncoding())
    {
        LOG_ERROR("Failed to initialize encoding");
        return false;
    }

    // Configurar SSL se habilitado E se Web Portal estiver habilitado
    // HTTPS só faz sentido se o Web Portal estiver ativo
    if (m_webPortalEnabled && m_enableHTTPS && !m_sslCertPath.empty() && !m_sslKeyPath.empty())
    {
        // Procura arquivos SSL: caminho absoluto > user-data/ssl > read-only assets > cwd.
        auto findSSLFile = [](const std::string &relativePath) -> std::string
        {
            fs::path testPath(relativePath);
            if (testPath.is_absolute() && fs::exists(testPath))
            {
                return fs::absolute(testPath).string();
            }

            fs::path inputPath(relativePath);
            std::string fileName = inputPath.filename();

            std::vector<std::string> possiblePaths;

            // 1. User data: certs do usuário (per-user, não roamed via XDG_DATA_HOME).
            std::string userDataDir = Paths::getUserDataDir();
            if (!userDataDir.empty())
            {
                possiblePaths.push_back((fs::path(userDataDir) / "ssl" / fileName).string());
            }

            // 2. Read-only assets: SSL bundle shipado com a aplicação.
            std::string roAssets = Paths::getReadOnlyAssetsDir();
            if (!roAssets.empty())
            {
                possiblePaths.push_back((fs::path(roAssets) / "ssl" / fileName).string());
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
                fs::path fsPath(path);
                if (fs::exists(fsPath) && fs::is_regular_file(fsPath))
                {
                    return fs::absolute(fsPath).string();
                }
            }

            return ""; // Não encontrado
        };

        // Extrair apenas o nome do arquivo do caminho fornecido
        fs::path certInputPath(m_sslCertPath);
        fs::path keyInputPath(m_sslKeyPath);
        std::string certFileName = certInputPath.filename();
        std::string keyFileName = keyInputPath.filename();

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
            LOG_ERROR("Searched in user-data/ssl, read-only assets/ssl, ./ssl/, ../ssl/, ../../ssl/");
            LOG_ERROR("Please generate certificates or disable HTTPS. Continuing with HTTP only.");
            m_enableHTTPS = false;
        }
        else if (foundKeyPath.empty())
        {
            LOG_ERROR("SSL Private Key file not found: " + m_sslKeyPath);
            LOG_ERROR("Searched in user-data/ssl, read-only assets/ssl, ./ssl/, ../ssl/, ../../ssl/");
            LOG_ERROR("Please generate certificates or disable HTTPS. Continuing with HTTP only.");
            m_enableHTTPS = false;
        }
        else
        {
            if (!m_httpServer.setSSLCertificate(foundCertPath, foundKeyPath))
            {
                LOG_ERROR("Failed to configure SSL certificate. Continuing with HTTP only.");
                m_enableHTTPS = false;
                m_foundSSLCertPath.clear();
                m_foundSSLKeyPath.clear();
            }
            else
            {
                // Armazenar caminhos encontrados para exibição na UI
                m_foundSSLCertPath = foundCertPath;
                m_foundSSLKeyPath = foundKeyPath;
            }
        }
    }
    else if (m_webPortalEnabled && m_enableHTTPS)
    {
        LOG_WARN("HTTPS habilitado mas certificados não configurados. Usando HTTP.");
        LOG_WARN("Cert path: " + m_sslCertPath + ", Key path: " + m_sslKeyPath);
        m_enableHTTPS = false;
    }

    // Se Web Portal estiver desabilitado, garantir que HTTPS também esteja desabilitado
    if (!m_webPortalEnabled && m_enableHTTPS)
    {
        m_enableHTTPS = false;
    }

    // Criar servidor HTTP/HTTPS
    if (!m_httpServer.createServer(m_port))
    {
        LOG_ERROR("Failed to create HTTP server");
        cleanupEncoding();
        return false;
    }

    // Configurar WebPortal para usar HTTPServer para envio de dados SSL
    m_webPortal.setHTTPServer(&m_httpServer);

    // Configurar APIController para usar HTTPServer
    m_apiController.setHTTPServer(&m_httpServer);

    // Iniciar threads
    m_running = true;
    m_active = true;
    m_stopRequest = false;

    m_serverThread = std::thread(&HTTPTSStreamer::serverThread, this);
    m_serverThread.detach();
    m_encodingThread = std::thread(&HTTPTSStreamer::encodingThread, this);
    m_encodingThread.detach();

    // Phase 2 of #47: parallel encoder for the /raw output. Idles when no
    // /raw clients are connected (Application.cpp gates pushes via
    // hasRawClients()), so the resting CPU cost is negligible.
    if (m_rawMediaEncoder.isInitialized())
    {
        m_rawEncodingThread = std::thread(&HTTPTSStreamer::rawEncodingThread, this);
        m_rawEncodingThread.detach();
    }

    return true;
}

bool HTTPTSStreamer::startWebPortalServer()
{
    // Este método inicia apenas o servidor HTTP/HTTPS sem o encoding thread
    // Usado quando o portal web está ativo mas o streaming não está

    if (m_active)
    {
        LOG_WARN("Servidor HTTP já está ativo");
        return true;
    }

    LOG_INFO("Iniciando servidor HTTP/HTTPS para Portal Web...");

    // Configurar HTTPS se necessário (mesma lógica do start())
    if (m_enableHTTPS && m_webPortalEnabled)
    {
        // Lambda para buscar arquivos SSL (mesma lógica do start())
        auto getUserConfigDir = []() -> std::string
        {
            const char *homeDir = std::getenv("HOME");
            if (homeDir)
            {
                fs::path configDir = fs::path(homeDir) / ".config" / "retrocapture";
                if (fs::exists(configDir))
                {
                    return configDir.string();
                }
            }
            return "";
        };

        auto findSSLFile = [getUserConfigDir](const std::string &relativePath) -> std::string
        {
            if (relativePath.empty())
                return "";

            fs::path inputPath(relativePath);
            std::string fileName = inputPath.filename();

            // Lista de locais para buscar (em ordem de prioridade)
            std::vector<std::string> possiblePaths;

            // 1. Pasta de configuração do usuário (~/.config/retrocapture/ssl/) - PRIORIDADE ALTA
            std::string userConfigDir = getUserConfigDir();
            if (!userConfigDir.empty())
            {
                fs::path userSSLDir = fs::path(userConfigDir) / "ssl";
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
                fs::path fsPath(path);
                if (fs::exists(fsPath) && fs::is_regular_file(fsPath))
                {
                    return fs::absolute(fsPath).string();
                }
            }

            return ""; // Não encontrado
        };

        // Extrair apenas o nome do arquivo do caminho fornecido
        fs::path certInputPath(m_sslCertPath);
        fs::path keyInputPath(m_sslKeyPath);
        std::string certFileName = certInputPath.filename();
        std::string keyFileName = keyInputPath.filename();

        // Buscar certificados SSL
        std::string foundCertPath = findSSLFile(m_sslCertPath);
        if (foundCertPath.empty())
        {
            foundCertPath = findSSLFile("ssl/" + certFileName);
            if (foundCertPath.empty())
            {
                foundCertPath = findSSLFile(certFileName);
            }
        }

        std::string foundKeyPath = findSSLFile(m_sslKeyPath);
        if (foundKeyPath.empty())
        {
            foundKeyPath = findSSLFile("ssl/" + keyFileName);
            if (foundKeyPath.empty())
            {
                foundKeyPath = findSSLFile(keyFileName);
            }
        }

        if (foundCertPath.empty() || foundKeyPath.empty())
        {
            LOG_WARN("HTTPS habilitado mas certificados não configurados. Usando HTTP.");
            m_enableHTTPS = false;
        }
        else
        {
            if (!m_httpServer.setSSLCertificate(foundCertPath, foundKeyPath))
            {
                LOG_ERROR("Failed to configure SSL certificate. Continuing with HTTP only.");
                m_enableHTTPS = false;
            }
            else
            {
                m_foundSSLCertPath = foundCertPath;
                m_foundSSLKeyPath = foundKeyPath;
            }
        }
    }

    // Criar servidor HTTP/HTTPS
    if (!m_httpServer.createServer(m_port))
    {
        LOG_ERROR("Failed to create HTTP server");
        return false;
    }

    // Configurar WebPortal para usar HTTPServer
    m_webPortal.setHTTPServer(&m_httpServer);

    // Configurar APIController para usar HTTPServer
    m_apiController.setHTTPServer(&m_httpServer);

    // Iniciar apenas a thread do servidor (sem encoding thread)
    m_running = true;
    m_active = true;
    m_stopRequest = false;

    m_serverThread = std::thread(&HTTPTSStreamer::serverThread, this);
    m_serverThread.detach();

    LOG_INFO("Servidor HTTP/HTTPS iniciado na porta " + std::to_string(m_port) + " (Portal Web apenas)");
    return true;
}

void HTTPTSStreamer::setSSLCertificatePath(const std::string &certPath, const std::string &keyPath)
{
    m_sslCertPath = certPath;
    m_sslKeyPath = keyPath;
    // Só habilitar HTTPS se Web Portal estiver habilitado
    // HTTPS só faz sentido se o Web Portal estiver ativo
    if (m_webPortalEnabled)
    {
        m_enableHTTPS = true;
    }
    else
    {
        m_enableHTTPS = false;
    }
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
        m_clientPending.clear();
        m_clientCount = 0;
    }

    // Phase 2 of #47: same for /raw clients.
    {
        std::lock_guard<std::mutex> lock(m_rawOutputMutex);
        for (int clientFd : m_rawClientSockets)
        {
            m_httpServer.closeClient(clientFd);
        }
        m_rawClientSockets.clear();
        m_rawClientPending.clear();
        m_rawClientCount = 0;
    }

    // Aguardar um tempo para threads detached processarem m_stopRequest e
    // terminarem. Os loops checam m_running/m_stopRequest a cada iteração,
    // que para os encoder threads é <30ms e para os client handlers é a
    // próxima recv (~10ms). 10s era exagero — congelava a UI quando o
    // restart era disparado por um callback de setting na thread principal.
    // 1.5s dá margem confortável sem trancar a interface.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Registrar timestamp de quando parou para cooldown
    {
        std::lock_guard<std::mutex> lock(m_stopTimeMutex);
        m_stopTime = std::chrono::steady_clock::now();
    }

    LOG_INFO("HTTP TS Streamer stopped");
}

bool HTTPTSStreamer::isActive() const
{
    return m_active;
}

bool HTTPTSStreamer::canStart() const
{
    // Se está ativo, pode "iniciar" (já está iniciado)
    if (m_active)
    {
        return true;
    }

    std::lock_guard<std::mutex> lock(m_stopTimeMutex);

    // Se nunca parou ou já passou o cooldown, pode iniciar
    if (m_stopTime == std::chrono::steady_clock::time_point())
    {
        return true;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_stopTime).count();
    return elapsed >= STOP_COOLDOWN_MS;
}

int64_t HTTPTSStreamer::getCooldownRemainingMs() const
{
    // Se está ativo, não há cooldown
    if (m_active)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_stopTimeMutex);

    // Se nunca parou, não há cooldown
    if (m_stopTime == std::chrono::steady_clock::time_point())
    {
        return 0;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_stopTime).count();
    auto remaining = STOP_COOLDOWN_MS - elapsed;
    return remaining > 0 ? remaining : 0;
}

std::string HTTPTSStreamer::getStreamUrl() const
{
    // HTTPS só está ativo se Web Portal estiver habilitado
    // Se portal estiver desabilitado, sempre usar HTTP
    if (!m_webPortalEnabled || !m_enableHTTPS)
    {
        return "http://localhost:" + std::to_string(m_port) + "/stream";
    }
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
#ifdef PLATFORM_LINUX
    int flag = 1;
    setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#elif defined(_WIN32)
    // Windows: usar int (BOOL é apenas um typedef para int)
    int flag = 1;
    setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, (const char *)&flag, sizeof(flag));
#endif

    // Read the HTTP request. We must loop until we see the header
    // terminator (\r\n\r\n) because TCP doesn't guarantee that an
    // entire request arrives in one segment — common when fronted
    // by Cloudflare or any tunnel where slow-start fragments the
    // request across multiple packets. The original code took the
    // single-recv shortcut, which on localhost always returned the
    // full request (loopback has huge MTU and one-shot delivery)
    // but on external paths returned 1-N bytes and the rest of the
    // request was abandoned, surfacing later as `Request preview: �`
    // garbage and broken asset loads in the portal.
    //
    // Cap the total bytes we'll accumulate (16 KB header limit) and
    // the total wait time (5 s) so a malicious or stuck client can't
    // hold a handler thread indefinitely.
    std::string request;
    request.reserve(2048);
    {
        char buffer[4096];
        constexpr size_t kMaxHeaderBytes = 16 * 1024;
        constexpr int    kRecvTimeoutMs  = 5000;

        // SO_RCVTIMEO so each recv has a deadline. Without it, a
        // peer that opens the connection and sends nothing would
        // park a thread forever.
#ifdef _WIN32
        DWORD tv = kRecvTimeoutMs;
        ::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char *>(&tv), sizeof(tv));
#else
        timeval tv{};
        tv.tv_sec  = kRecvTimeoutMs / 1000;
        tv.tv_usec = (kRecvTimeoutMs % 1000) * 1000;
        ::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

        while (request.size() < kMaxHeaderBytes)
        {
            ssize_t n = m_httpServer.receiveData(clientFd, buffer,
                                                  sizeof(buffer) - 1);
            if (n <= 0)
            {
                // 0 = peer closed, <0 = error or timeout. If we have
                // partial data buffered there's no point trying to
                // parse it as a valid HTTP request.
                m_httpServer.closeClient(clientFd);
                return;
            }
            buffer[n] = '\0';
            request.append(buffer, static_cast<size_t>(n));
            // Headers fully arrived?
            if (request.find("\r\n\r\n") != std::string::npos) break;
        }
        if (request.find("\r\n\r\n") == std::string::npos)
        {
            // Headers larger than 16 KB or stream never produced a
            // terminator — either malformed or hostile. Drop.
            m_httpServer.closeClient(clientFd);
            return;
        }

        // The SO_RCVTIMEO we set above is socket-wide and persists
        // for the rest of this connection. That's fine for the read
        // phase (and would auto-cap a stuck request) but BAD for the
        // /stream monitoring loop later, where recv is intentionally
        // a long-living 'detect disconnect' watch — there a 5 s
        // EAGAIN would be mis-read as a peer-close and tear the
        // stream down every 5 s. Reset the timeout to unlimited
        // (tv = 0) before we leave this block.
#ifdef _WIN32
        DWORD tvOff = 0;
        ::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char *>(&tvOff), sizeof(tvOff));
#else
        timeval tvOff{};
        tvOff.tv_sec  = 0;
        tvOff.tv_usec = 0;
        ::setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tvOff, sizeof(tvOff));
#endif
    }
    ssize_t bytesRead = static_cast<ssize_t>(request.size());
    (void)bytesRead;

    // Se HTTPS está habilitado E Web Portal está habilitado mas o cliente está usando HTTP, redirecionar para HTTPS
    // HTTPS só faz sentido se o Web Portal estiver ativo
    if (m_webPortalEnabled && m_enableHTTPS && !m_httpServer.isClientHTTPS(clientFd))
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

        m_httpServer.sendData(clientFd, redirectResponse.c_str(), redirectResponse.length());
        m_httpServer.closeClient(clientFd);
        return;
    }

    // Top-level password gate. When the user has set a stream
    // password (via the publish UI), EVERYTHING on this HTTP server
    // needs auth — the portal HTML, the static assets, the live
    // /stream feed, the /api routes, and the directory-facing
    // /raw + /meta endpoints alike. Without this gate, a passworded
    // stream would still let any external visitor see the live video
    // by opening the portal URL in a browser; the password would
    // only protect the directory-side endpoints. That's a leak.
    //
    // Two auth schemes are accepted via HttpAuth::authorizedAnyScheme:
    //   - Authorization: Bearer <sha256(password)>  (RetroCapture
    //     client, mpegts.js with custom header, programmatic users)
    //   - Authorization: Basic base64("user:password")  (browser's
    //     native popup; the username portion is ignored)
    //
    // On reject we send 401 + WWW-Authenticate: Basic so the browser
    // pops its credentials dialog and remembers the answer for the
    // rest of the session.
    {
        std::string pwHash;
        {
            std::lock_guard<std::mutex> lock(m_passwordMu);
            pwHash = m_streamPasswordHash;
        }
        if (!pwHash.empty() && !HttpAuth::authorizedAnyScheme(request, pwHash))
        {
            const char *response =
                "HTTP/1.1 401 Unauthorized\r\n"
                "WWW-Authenticate: Basic realm=\"RetroCapture\"\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n"
                "Connection: close\r\n"
                "\r\n"
                "Password required.";
            m_httpServer.sendData(clientFd, response, strlen(response));
            m_httpServer.closeClient(clientFd);
            return;
        }
    }

    // Verificar tipo de requisição (ANTES de verificar portal web).
    //
    // Both classifications match against the REQUEST-LINE PATH only —
    // never against the full request string. The original isStreamRequest
    // used `request.find("/stream")` against the whole blob, which
    // false-matched for users whose host header / Referer happened to
    // contain that substring. Concretely, deploying behind a hostname
    // like `stream.retrocapture.com` made every sub-resource request
    // (which carries `Referer: https://stream.retrocapture.com/`) light
    // up as a stream request — assets ended up served by the MPEG-TS
    // handler with Content-Type: video/mp2t, the browser refused to
    // execute them, and the page hung waiting for never-ending data.
    bool isStreamRequest = false;
    bool isRawRequest = false;
    {
        size_t methodEnd = request.find(' ');
        if (methodEnd != std::string::npos)
        {
            size_t pathEnd = request.find(' ', methodEnd + 1);
            if (pathEnd != std::string::npos)
            {
                std::string path = request.substr(methodEnd + 1, pathEnd - methodEnd - 1);
                size_t q = path.find('?');
                if (q != std::string::npos) path = path.substr(0, q);
                isStreamRequest = (path == "/stream" || path.rfind("/stream/", 0) == 0);
                isRawRequest    = (path == "/raw");
            }
        }
    }

    // Extrair prefixo base para verificar requisições com prefixo
    // Prioridade: 1) X-Forwarded-Prefix header, 2) path da requisição
    std::string basePrefixForDetection = "";

    // 1. Tentar extrair do header X-Forwarded-Prefix (padrão para proxy reverso)
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
                basePrefixForDetection = prefix;
            }
        }
    }

    // 2. Se ainda vazio, tentar extrair do path da requisição
    if (basePrefixForDetection.empty())
    {
        size_t getPos = request.find("GET /");
        if (getPos != std::string::npos)
        {
            size_t startPos = getPos + 5;
            size_t endPos = request.find(" ", startPos);
            if (endPos == std::string::npos)
            {
                endPos = request.find("\r\n", startPos);
            }
            if (endPos != std::string::npos)
            {
                std::string path = request.substr(startPos, endPos - startPos);
                // Remover query string
                size_t queryPos = path.find('?');
                if (queryPos != std::string::npos)
                {
                    path = path.substr(0, queryPos);
                }
                // Extrair prefixo se houver (ex: /retrocapture/stream.m3u8 -> /retrocapture)
                // Verificar se o path tem mais de um segmento (indicando prefixo base)
                if (path.length() > 1 && path[0] == '/')
                {
                    // Contar quantas barras existem
                    size_t slashCount = 0;
                    for (char c : path)
                    {
                        if (c == '/')
                            slashCount++;
                    }
                    // Se houver mais de uma barra, há um prefixo base
                    if (slashCount > 1)
                    {
                        size_t secondSlash = path.find('/', 1);
                        if (secondSlash != std::string::npos)
                        {
                            basePrefixForDetection = path.substr(0, secondSlash);
                        }
                    }
                }
            }
        }
    }

    // Verificar API REST primeiro (antes do Web Portal)
    if (m_apiController.isAPIRequest(request))
    {
        if (m_apiController.handleRequest(clientFd, request))
        {
            m_httpServer.closeClient(clientFd);
            return;
        }
        // Fall through to other checks if not handled.
    }

    // Only consult the web portal when this is neither a stream nor /raw
    // request and the portal is enabled.
    if (m_webPortalEnabled && !isStreamRequest && !isRawRequest)
    {
        if (m_webPortal.isWebPortalRequest(request))
        {
            if (m_webPortal.handleRequest(clientFd, request))
            {
                m_httpServer.closeClient(clientFd);
                return;
            }
            // Fall through to other checks if not handled.
        }
    }

    if (!isStreamRequest && !isRawRequest)
    {
        send404(clientFd);
        m_httpServer.closeClient(clientFd);
        return;
    }

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

    // Phase 2 of #47: branch on /raw vs /stream — same protocol on the wire,
    // different state mirrors (format header, client list, mutex).
    if (isRawRequest)
    {
        // #49 Phase 3 — password gate. When the user has configured a
        // stream password, /raw rejects unauthenticated connections
        // with 401 + WWW-Authenticate so any CLI / FFmpeg consumer
        // can surface the missing-token failure cleanly. /stream is
        // intentionally not gated — it stays open for VLC / mpv /
        // browser portal viewers.
        std::string pwHash;
        {
            std::lock_guard<std::mutex> lock(m_passwordMu);
            pwHash = m_streamPasswordHash;
        }
        if (!HttpAuth::authorized(request, pwHash))
        {
            const char *response = "HTTP/1.1 401 Unauthorized\r\n"
                                   "WWW-Authenticate: Bearer realm=\"retrocapture-raw\"\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "This stream requires a password. "
                                   "Send Authorization: Bearer <sha256(password)> "
                                   "or append ?token=<sha256(password)> to the URL.";
            m_httpServer.sendData(clientFd, response, strlen(response));
            m_httpServer.closeClient(clientFd);
            return;
        }

        // /raw is only meaningful while streaming is actually running —
        // the raw encoder pipeline is brought up by initializeRawPipeline
        // as part of start() and torn down by stop(). Without it, the
        // HTTP socket accepts the connection and ffmpeg/avformat_open_input
        // sees a valid TS-ish header (whatever the muxer flushed before
        // shutdown) but no further packets ever arrive — the user-visible
        // symptom is "client says connected but stream stays black".
        // Bail with 503 so the client treats it as a failed connect.
        if (!m_rawMediaEncoder.isInitialized())
        {
            const char *response = "HTTP/1.1 503 Service Unavailable\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "Streaming is not running on this server. "
                                   "Start streaming on the host before connecting.";
            m_httpServer.sendData(clientFd, response, strlen(response));
            m_httpServer.closeClient(clientFd);
            return;
        }
        {
            std::lock_guard<std::mutex> headerLock(m_rawHeaderMutex);
            if (m_rawHeaderWritten && !m_rawFormatHeader.empty())
            {
                ssize_t headerSent = m_httpServer.sendData(clientFd, m_rawFormatHeader.data(), m_rawFormatHeader.size());
                if (headerSent < 0)
                {
                    LOG_ERROR("Failed to send raw format header to client");
                    m_httpServer.closeClient(clientFd);
                    return;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_rawOutputMutex);
            m_rawClientSockets.push_back(clientFd);
            m_rawClientCount = m_rawClientSockets.size();
        }

        while (!m_stopRequest && m_running)
        {
            char dummy;
            ssize_t result = m_httpServer.receiveData(clientFd, &dummy, 1);
            if (result <= 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Atomically remove from the broadcast list AND close the fd,
        // both under the list lock. Without this writeToRawClients
        // (which also detects send failures and closes the fd) and the
        // handler thread could double-close the same fd value — and
        // because Linux reuses fd numbers immediately, the second close
        // sometimes shut down a freshly-accepted unrelated connection,
        // breaking every subsequent client until streaming was
        // restarted. Whichever path finds the fd still in the list
        // wins the close; the loser sees an empty find result and exits
        // without touching the fd.
        {
            std::lock_guard<std::mutex> lock(m_rawOutputMutex);
            auto it = std::find(m_rawClientSockets.begin(), m_rawClientSockets.end(), clientFd);
            if (it != m_rawClientSockets.end())
            {
                m_rawClientSockets.erase(it);
                m_rawClientPending.erase(clientFd);
                m_rawClientCount = m_rawClientSockets.size();
                m_httpServer.closeClient(clientFd);
            }
        }
        return;
    }

    // /stream path — unchanged from before Phase 2.

    // Enviar header do formato MPEG-TS se já foi capturado
    {
        std::lock_guard<std::mutex> headerLock(m_headerMutex);
        if (m_headerWritten && !m_formatHeader.empty())
        {
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
        }
    }

    // Adicionar cliente à lista
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        m_clientSockets.push_back(clientFd);
        m_clientCount = m_clientSockets.size();
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

    // Atomically remove from the broadcast list AND close the fd, same
    // pattern as the /raw branch above — avoids the double-close race
    // with writeToClients on disconnect.
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        auto it = std::find(m_clientSockets.begin(), m_clientSockets.end(), clientFd);
        if (it != m_clientSockets.end())
        {
            m_clientSockets.erase(it);
            m_clientPending.erase(clientFd);
            m_clientCount = m_clientSockets.size();
            m_httpServer.closeClient(clientFd);
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

int HTTPTSStreamer::writeToClients(const uint8_t *buf, int buf_size)
{
    if (!buf || buf_size <= 0 || m_stopRequest)
    {
        return buf_size; // Retornar sucesso para não bloquear FFmpeg
    }

    // Header do formato é capturado automaticamente pelo MediaMuxer
    // Apenas sincronizar se necessário
    {
        std::lock_guard<std::mutex> headerLock(m_headerMutex);
        if (!m_headerWritten && m_mediaMuxer.isHeaderWritten())
        {
            m_formatHeader = m_mediaMuxer.getFormatHeader();
            m_headerWritten = true;
        }
    }

    // Broadcast to every connected client. EAGAIN / partial sends used
    // to silently drop the unsent tail; that corrupts MPEG-TS framing
    // and mpegts.js loses the SourceBuffer permanently. We now stash
    // the leftover bytes in a per-client tail buffer and drain it on
    // the next call so byte order is preserved across brief slow-downs.
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);

        if (m_stopRequest || m_clientSockets.empty())
        {
            return buf_size;
        }

        auto it = m_clientSockets.begin();
        while (it != m_clientSockets.end())
        {
            const int clientFd = *it;
            ClientPending &p = m_clientPending[clientFd]; // default-construct if missing
            bool drop = false;

            // 1. Try to drain any pending bytes from a previous call first.
            //    Order matters: we cannot send 'buf' before the existing
            //    tail or the receiver sees a discontinuity.
            while (p.pending() > 0)
            {
                ssize_t sent = m_httpServer.sendData(
                    clientFd,
                    p.tail.data() + p.tailOffset,
                    p.pending());
                if (sent < 0) { drop = true; break; }
                if (sent == 0) break; // EAGAIN — kernel buffer still full
                p.tailOffset += static_cast<size_t>(sent);
                if (p.tailOffset >= p.tail.size())
                {
                    p.tail.clear();
                    p.tailOffset = 0;
                }
            }

            // 2. If the tail is empty, try the new payload directly.
            size_t newOffset = 0;
            if (!drop && p.pending() == 0)
            {
                while (newOffset < static_cast<size_t>(buf_size))
                {
                    ssize_t sent = m_httpServer.sendData(
                        clientFd,
                        buf + newOffset,
                        static_cast<size_t>(buf_size) - newOffset);
                    if (sent < 0) { drop = true; break; }
                    if (sent == 0) break; // EAGAIN
                    newOffset += static_cast<size_t>(sent);
                }
            }

            // 3. Stash anything we couldn't send so the next call resumes
            //    from exactly where we left off.
            if (!drop)
            {
                if (p.pending() > 0)
                {
                    // Couldn't drain the old tail — append the new payload
                    // whole, otherwise the receiver would see buf before
                    // the still-queued previous packet.
                    p.tail.insert(p.tail.end(), buf, buf + buf_size);
                }
                else if (newOffset < static_cast<size_t>(buf_size))
                {
                    p.tail.assign(buf + newOffset, buf + buf_size);
                    p.tailOffset = 0;
                }

                // Compact the partially-consumed prefix once it gets
                // chunky so the tail doesn't grow unbounded just from
                // offset accounting.
                if (p.tailOffset > 64 * 1024)
                {
                    p.tail.erase(p.tail.begin(),
                                 p.tail.begin() + static_cast<std::ptrdiff_t>(p.tailOffset));
                    p.tailOffset = 0;
                }

                // Hopelessly slow client: backlog past the bound means
                // the receiver has been falling behind for seconds. Cut
                // them rather than burn memory forever.
                if (p.pending() > kMaxClientBacklog)
                {
                    LOG_WARN("/stream client fd=" + std::to_string(clientFd) +
                             " send backlog " + std::to_string(p.pending()) +
                             " bytes exceeded cap (" + std::to_string(kMaxClientBacklog) +
                             ") — closing");
                    drop = true;
                }
            }

            if (drop)
            {
                m_httpServer.closeClient(clientFd);
                m_clientPending.erase(clientFd);
                it = m_clientSockets.erase(it);
                m_clientCount = m_clientSockets.size();
            }
            else
            {
                ++it;
            }
        }

        return buf_size;
    }
}

int HTTPTSStreamer::writeToRawClients(const uint8_t *buf, int buf_size)
{
    // Mirror of writeToClients, operating on the /raw output state. Kept as
    // a near-duplicate so the /stream path stays byte-for-byte unchanged.
    if (!buf || buf_size <= 0 || m_stopRequest)
    {
        return buf_size;
    }

    {
        std::lock_guard<std::mutex> headerLock(m_rawHeaderMutex);
        if (!m_rawHeaderWritten && m_rawMediaMuxer.isHeaderWritten())
        {
            m_rawFormatHeader = m_rawMediaMuxer.getFormatHeader();
            m_rawHeaderWritten = true;
        }
    }

    // Same tail-buffer logic as writeToClients — see the long comment
    // there. /raw clients are FFmpeg-driven and rarely hit backpressure
    // in practice (no Cloudflare hop), but the corruption mode is
    // identical so we apply the same fix for symmetry.
    {
        std::lock_guard<std::mutex> lock(m_rawOutputMutex);
        if (m_stopRequest || m_rawClientSockets.empty())
        {
            return buf_size;
        }

        auto it = m_rawClientSockets.begin();
        while (it != m_rawClientSockets.end())
        {
            const int clientFd = *it;
            ClientPending &p = m_rawClientPending[clientFd];
            bool drop = false;

            while (p.pending() > 0)
            {
                ssize_t sent = m_httpServer.sendData(
                    clientFd,
                    p.tail.data() + p.tailOffset,
                    p.pending());
                if (sent < 0) { drop = true; break; }
                if (sent == 0) break;
                p.tailOffset += static_cast<size_t>(sent);
                if (p.tailOffset >= p.tail.size())
                {
                    p.tail.clear();
                    p.tailOffset = 0;
                }
            }

            size_t newOffset = 0;
            if (!drop && p.pending() == 0)
            {
                while (newOffset < static_cast<size_t>(buf_size))
                {
                    ssize_t sent = m_httpServer.sendData(
                        clientFd,
                        buf + newOffset,
                        static_cast<size_t>(buf_size) - newOffset);
                    if (sent < 0) { drop = true; break; }
                    if (sent == 0) break;
                    newOffset += static_cast<size_t>(sent);
                }
            }

            if (!drop)
            {
                if (p.pending() > 0)
                {
                    p.tail.insert(p.tail.end(), buf, buf + buf_size);
                }
                else if (newOffset < static_cast<size_t>(buf_size))
                {
                    p.tail.assign(buf + newOffset, buf + buf_size);
                    p.tailOffset = 0;
                }
                if (p.tailOffset > 64 * 1024)
                {
                    p.tail.erase(p.tail.begin(),
                                 p.tail.begin() + static_cast<std::ptrdiff_t>(p.tailOffset));
                    p.tailOffset = 0;
                }
                if (p.pending() > kMaxClientBacklog)
                {
                    LOG_WARN("/raw client fd=" + std::to_string(clientFd) +
                             " send backlog " + std::to_string(p.pending()) +
                             " bytes exceeded cap — closing");
                    drop = true;
                }
            }

            if (drop)
            {
                m_httpServer.closeClient(clientFd);
                m_rawClientPending.erase(clientFd);
                it = m_rawClientSockets.erase(it);
                m_rawClientCount = m_rawClientSockets.size();
            }
            else
            {
                ++it;
            }
        }

        return buf_size;
    }
}

bool HTTPTSStreamer::initializeEncoding()
{
    // Configurar MediaSynchronizer com valores configuráveis
    m_streamSynchronizer.setMaxBufferTime(m_maxBufferTimeSeconds * 1000000LL);
    m_streamSynchronizer.setMaxVideoBufferSize(m_maxVideoBufferSize);
    m_streamSynchronizer.setMaxAudioBufferSize(m_maxAudioBufferSize);
    m_streamSynchronizer.setSyncTolerance(50 * 1000LL); // 50ms

    // Configurar MediaEncoder
    MediaEncoder::VideoConfig videoConfig;
    videoConfig.width = m_width;
    videoConfig.height = m_height;
    videoConfig.fps = m_fps;
    videoConfig.bitrate = m_videoBitrate;
    videoConfig.codec = m_videoCodecName;
    videoConfig.preset = (m_videoCodecName == "h264" || m_videoCodecName == "libx264") ? m_h264Preset : m_h265Preset;
    videoConfig.profile = (m_videoCodecName == "h264" || m_videoCodecName == "libx264") ? "baseline" : "";
    videoConfig.h265Profile = m_h265Profile;
    videoConfig.h265Level = m_h265Level;
    videoConfig.vp8Speed = m_vp8Speed;
    videoConfig.vp9Speed = m_vp9Speed;
    videoConfig.hardwareEncoder = m_hardwareEncoder;
    videoConfig.hwPreset = m_hardwareEncoderPreset;

    MediaEncoder::AudioConfig audioConfig;
    audioConfig.sampleRate = m_audioSampleRate;
    audioConfig.channels = m_audioChannelsCount;
    audioConfig.bitrate = m_audioBitrate;
    audioConfig.codec = m_audioCodecName;

    LOG_INFO("Inicializando MediaEncoder (codec=" + videoConfig.codec + ", " +
             std::to_string(videoConfig.width) + "x" + std::to_string(videoConfig.height) +
             "@" + std::to_string(videoConfig.fps) + "fps)");

    // Inicializar encoder para streaming (usa repeat-headers)
    if (!m_mediaEncoder.initialize(videoConfig, audioConfig, true))
    {
        LOG_ERROR("Failed to initialize MediaEncoder");
        return false;
    }

    LOG_INFO("MediaEncoder inicializado com sucesso");

    // Configurar MediaMuxer com callback de escrita
    auto writeCallback = [this](const uint8_t *data, size_t size) -> int
    {
        return this->writeToClients(data, size);
    };

    LOG_INFO("Inicializando MediaMuxer (avioBufferSize=" + std::to_string(m_avioBufferSize) + ")");

    if (!m_mediaMuxer.initialize(videoConfig, audioConfig,
                                 m_mediaEncoder.getVideoCodecContext(),
                                 m_mediaEncoder.getAudioCodecContext(),
                                 "", // filePath vazio para streaming (usa callback)
                                 writeCallback,
                                 m_avioBufferSize))
    {
        LOG_ERROR("Failed to initialize MediaMuxer");
        m_mediaEncoder.cleanup();
        return false;
    }

    LOG_INFO("MediaMuxer inicializado com sucesso - encoding pronto para streaming");

    // Phase 2 of #47: also bring up the /raw pipeline so RetroCapture remote
    // clients can consume the pre-shader feed. Soft-fail — if /raw can't
    // come up, log it and keep /stream working.
    if (!initializeRawPipeline())
    {
        LOG_WARN("Failed to initialize /raw pipeline — /stream still functional");
    }

    return true;
}

bool HTTPTSStreamer::initializeRawPipeline()
{
    // The /raw synchronizer gets noticeably bigger buffers than /stream
    // (~1 s of video and ~1 s of audio). The /raw consumer is the
    // dedicated raw-encoder thread; bursts in the push side or brief
    // hiccups in the encoder shouldn't show up as drops on a fed-by-
    // identical-source pipeline whose only consumer is supposed to keep
    // up. The bigger buffer absorbs those bursts without imposing
    // meaningful latency — the encoder still pulls in near-real-time.
    const size_t rawMaxVideo = std::max<size_t>(m_maxVideoBufferSize, 60);
    const size_t rawMaxAudio = std::max<size_t>(m_maxAudioBufferSize, 120);
    m_rawStreamSynchronizer.setMaxBufferTime(m_maxBufferTimeSeconds * 1000000LL);
    m_rawStreamSynchronizer.setMaxVideoBufferSize(rawMaxVideo);
    m_rawStreamSynchronizer.setMaxAudioBufferSize(rawMaxAudio);
    m_rawStreamSynchronizer.setSyncTolerance(50 * 1000LL);

    // Strategy B (#47): /raw shares codec config (bitrate, codec, preset)
    // with /stream. Separate encoder/muxer state lives parallel below.
    MediaEncoder::VideoConfig videoConfig;
    videoConfig.width    = m_width;
    videoConfig.height   = m_height;
    videoConfig.fps      = m_fps;
    videoConfig.bitrate  = m_videoBitrate;
    videoConfig.codec    = m_videoCodecName;
    videoConfig.preset   = (m_videoCodecName == "h264" || m_videoCodecName == "libx264") ? m_h264Preset : m_h265Preset;
    videoConfig.profile  = (m_videoCodecName == "h264" || m_videoCodecName == "libx264") ? "baseline" : "";
    videoConfig.h265Profile = m_h265Profile;
    videoConfig.h265Level   = m_h265Level;
    videoConfig.vp8Speed    = m_vp8Speed;
    videoConfig.vp9Speed    = m_vp9Speed;
    videoConfig.hardwareEncoder = m_hardwareEncoder;
    videoConfig.hwPreset = m_hardwareEncoderPreset;

    MediaEncoder::AudioConfig audioConfig;
    audioConfig.sampleRate = m_audioSampleRate;
    audioConfig.channels   = m_audioChannelsCount;
    audioConfig.bitrate    = m_audioBitrate;
    audioConfig.codec      = m_audioCodecName;

    LOG_INFO("Initializing /raw MediaEncoder (same config as /stream)");

    if (!m_rawMediaEncoder.initialize(videoConfig, audioConfig, true))
    {
        LOG_ERROR("Failed to initialize raw MediaEncoder");
        return false;
    }

    auto rawWriteCallback = [this](const uint8_t *data, size_t size) -> int
    {
        return this->writeToRawClients(data, size);
    };

    if (!m_rawMediaMuxer.initialize(videoConfig, audioConfig,
                                    m_rawMediaEncoder.getVideoCodecContext(),
                                    m_rawMediaEncoder.getAudioCodecContext(),
                                    "", // streaming via callback
                                    rawWriteCallback,
                                    m_avioBufferSize))
    {
        LOG_ERROR("Failed to initialize raw MediaMuxer");
        m_rawMediaEncoder.cleanup();
        return false;
    }

    LOG_INFO("Raw pipeline initialized — /raw ready to serve pre-shader frames");
    return true;
}

void HTTPTSStreamer::cleanupRawPipeline()
{
    if (m_rawMediaEncoder.isInitialized())
    {
        std::vector<MediaEncoder::EncodedPacket> packets;
        m_rawMediaEncoder.flush(packets);
        for (const auto &p : packets)
        {
            m_rawMediaMuxer.muxPacket(p);
        }
    }
    if (m_rawMediaMuxer.isInitialized())
    {
        m_rawMediaMuxer.flush();
    }
    m_rawMediaMuxer.cleanup();
    m_rawMediaEncoder.cleanup();
    m_rawStreamSynchronizer.clear();

    {
        std::lock_guard<std::mutex> lock(m_rawHeaderMutex);
        m_rawFormatHeader.clear();
        m_rawHeaderWritten = false;
    }
}

void HTTPTSStreamer::cleanupEncoding()
{
    // Flush encoder para processar frames pendentes
    if (m_mediaEncoder.isInitialized())
    {
        std::vector<MediaEncoder::EncodedPacket> packets;
        m_mediaEncoder.flush(packets);

        // Muxar pacotes pendentes
        for (const auto &packet : packets)
        {
            m_mediaMuxer.muxPacket(packet);
        }
    }

    // Flush muxer
    if (m_mediaMuxer.isInitialized())
    {
        m_mediaMuxer.flush();
    }

    // Limpar recursos
    m_mediaMuxer.cleanup();
    m_mediaEncoder.cleanup();
    m_streamSynchronizer.clear();

    // Capturar header do formato se disponível
    {
        std::lock_guard<std::mutex> lock(m_headerMutex);
        if (m_mediaMuxer.isHeaderWritten())
        {
            m_formatHeader = m_mediaMuxer.getFormatHeader();
            m_headerWritten = true;
        }
        else
        {
            m_formatHeader.clear();
            m_headerWritten = false;
        }
    }

    // Phase 2 of #47: also tear down the /raw pipeline.
    cleanupRawPipeline();
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
    FFmpegCompat::setChannelLayout(codecCtx, m_audioChannelsCount);
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

#if LIBAVCODEC_VERSION_MAJOR >= 59
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
#else
    av_opt_set_int(swrCtx, "in_channel_layout", av_get_default_channel_layout(m_audioChannelsCount), 0);
    av_opt_set_int(swrCtx, "in_sample_rate", static_cast<int>(m_audioSampleRate), 0);
    av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    av_opt_set_int(swrCtx, "out_channel_layout", av_get_default_channel_layout(m_audioChannelsCount), 0);
    av_opt_set_int(swrCtx, "out_sample_rate", static_cast<int>(m_audioSampleRate), 0);
    av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);

    if (swr_init(swrCtx) < 0)
    {
        LOG_ERROR("Failed to initialize SWR context");
        swr_free(&swrCtx);
        avcodec_free_context(&codecCtx);
        m_audioCodecContext = nullptr;
        return false;
    }
#endif
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
    FFmpegCompat::setFrameChannelLayout(audioFrame, m_audioChannelsCount);
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

    // Configurar callback de escrita com tamanho configurável
    // NOTA: Esta função initializeMuxers() parece ser código legado - o streaming agora usa MediaMuxer
    // Mas mantemos para compatibilidade
    // Converter bufferSize para int (como em MediaMuxer.cpp linha 118)
    const size_t bufferSize = m_avioBufferSize;
    unsigned char *avioBuffer = static_cast<unsigned char *>(av_malloc(bufferSize));
    if (!avioBuffer)
    {
        LOG_ERROR("Failed to allocate AVIO buffer");
        av_free(const_cast<char *>(formatCtx->url));
        avformat_free_context(formatCtx);
        return false;
    }
    // Usar o callback estático definido no topo do arquivo
    // MediaMuxer.cpp linha 118 usa: avio_alloc_context(..., bufferSize, ...) onde bufferSize é size_t
    // Vamos fazer exatamente o mesmo - FFmpeg aceita size_t sendo convertido para int
    // NOTA: Esta função initializeMuxers() é código legado - o streaming agora usa MediaMuxer
    // Mas mantemos para compatibilidade. Se houver erro de compilação aqui, pode ser ignorado
    // pois esta função não é mais chamada (o streaming usa MediaMuxer via initializeEncoding())
    // Usar callback apropriado baseado na arquitetura
    // Diferentes versões do FFmpeg esperam assinaturas diferentes:
    // - FFmpeg 6.1+ (libavformat 61+): const uint8_t*
    // - FFmpeg 6.0- (libavformat 60-): uint8_t* (não const)
    #if FFMPEG_USE_CONST_WRITE_CALLBACK
        // FFmpeg 6.1+: usar callback com const uint8_t*
        formatCtx->pb = avio_alloc_context(
            avioBuffer, static_cast<int>(bufferSize),
            1, this, nullptr, ::writeCallback, nullptr);
    #else
        // FFmpeg 6.0-: usar wrapper com uint8_t* (não const)
        formatCtx->pb = avio_alloc_context(
            avioBuffer, static_cast<int>(bufferSize),
            1, this, nullptr, ::writeCallbackNonConst, nullptr);
    #endif
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
                FFmpegCompat::setKeyFrame(dummyFrame, true);

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
        FFmpegCompat::setKeyFrame(videoFrame, true);
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
    // MediaSynchronizer já faz cleanup internamente
    m_streamSynchronizer.cleanupOldData();
}

void HTTPTSStreamer::serverThread()
{

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

    return;
}

void HTTPTSStreamer::encodingThread()
{
    // Aguardar um pouco antes de começar para evitar processar frames muito antigos
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Contador para limpeza menos frequente (a cada 10 iterações)
    int cleanupCounter = 0;
    const int CLEANUP_INTERVAL = 10;

    while (m_running)
    {
        if (m_stopRequest)
        {
            break;
        }

        bool processedAny = false;

        // Limpar dados antigos do MediaSynchronizer apenas ocasionalmente (não a cada iteração)
        // Isso evita remover dados muito agressivamente antes de serem processados
        cleanupCounter++;
        if (cleanupCounter >= CLEANUP_INTERVAL)
        {
            m_streamSynchronizer.cleanupOldData();
            cleanupCounter = 0;
        }

        // Drain audio independently of the sync zone. AAC is cheap and
        // doesn't need to wait for video — same fix applied to recording
        // in #34. Without this, slow video iterations let the audio buffer
        // overflow and chunks get dropped at the synchronizer; since the
        // audio PTS is sample-count based, the resulting stream's audio
        // ends up shorter than its video.
        {
            auto pendingAudio = m_streamSynchronizer.getAllUnprocessedAudio();
            for (const auto &chunk : pendingAudio)
            {
                if (m_stopRequest) break;
                if (chunk.processed || !chunk.samples || chunk.sampleCount == 0) continue;

                std::vector<MediaEncoder::EncodedPacket> aPackets;
                if (m_mediaEncoder.encodeAudio(chunk.samples->data(), chunk.sampleCount,
                                               chunk.captureTimestampUs, aPackets))
                {
                    for (const auto &p : aPackets)
                    {
                        m_mediaMuxer.muxPacket(p);
                    }
                    processedAny = true;
                }
                m_streamSynchronizer.markAudioChunkProcessedByTimestamp(chunk.captureTimestampUs);
            }
        }

        size_t videoBufferSize = m_streamSynchronizer.getVideoBufferSize();
        size_t audioBufferSize = m_streamSynchronizer.getAudioBufferSize();
        bool hasBacklog = (videoBufferSize > 5 || audioBufferSize > 10);

        MediaSynchronizer::SyncZone syncZone = m_streamSynchronizer.calculateSyncZone();

        if (syncZone.isValid())
        {
            // Obter frames de vídeo da zona sincronizada
            auto videoFrames = m_streamSynchronizer.getVideoFrames(syncZone);

            // Processar frames de vídeo (com controle de taxa para evitar aceleração)
            // Aumentar limite quando há backlog para evitar perda de frames
            size_t framesProcessed = 0;
            // Bigger catchup batch under backlog — when the bound on
            // frames-per-iteration is too small the encoder thread can't
            // drain a transient spike before the next push, the
            // synchronizer fills, and addVideoFrame starts dropping at
            // the source even though the encoder is well within its
            // throughput budget. 30 covers a full second of vsync push
            // in a single iteration.
            size_t MAX_FRAMES_PER_ITERATION = hasBacklog ? 30 : 2;

            for (const auto &frame : videoFrames)
            {
                if (m_stopRequest)
                {
                    break;
                }

                // Limitar número de frames processados por iteração
                if (framesProcessed >= MAX_FRAMES_PER_ITERATION)
                {
                    break;
                }

                if (!frame.processed && frame.data && frame.width > 0 && frame.height > 0)
                {
                    // Encodar frame usando MediaEncoder
                    std::vector<MediaEncoder::EncodedPacket> packets;
                    if (m_mediaEncoder.encodeVideo(frame.data->data(), frame.width, frame.height,
                                                   frame.captureTimestampUs, packets))
                    {
                        // Muxar pacotes usando MediaMuxer
                        for (const auto &packet : packets)
                        {
                            m_mediaMuxer.muxPacket(packet);
                        }
                        processedAny = true;
                        framesProcessed++;
                        // Mark this specific frame as processed by its
                        // capture timestamp — see the matching note in
                        // rawEncodingThread for why index-range marking
                        // was wrong (it marked the first N slots of the
                        // sync zone, but the loop skips already-
                        // processed frames so the actually-encoded ones
                        // were not at those positions and got re-encoded
                        // on subsequent iterations).
                        m_streamSynchronizer.markVideoFrameProcessedByTimestamp(frame.captureTimestampUs);
                    }
                }
            }

            // Audio was already drained at the top of the loop, no need
            // to process it again here.

            // Capturar header do formato se disponível
            {
                std::lock_guard<std::mutex> lock(m_headerMutex);
                if (!m_headerWritten && m_mediaMuxer.isHeaderWritten())
                {
                    m_formatHeader = m_mediaMuxer.getFormatHeader();
                    m_headerWritten = true;
                }
            }
        }

        // Adicionar delay mínimo entre iterações para evitar aceleração
        // Reduzir ou remover delay quando há backlog para processar mais rápido
        if (processedAny)
        {
            if (hasBacklog)
            {
                // Quando há backlog, delay mínimo para não consumir 100% CPU mas processar rápido
                std::this_thread::sleep_for(std::chrono::microseconds(100)); // 100µs apenas
            }
            else
            {
                // Sem backlog: delay baseado no FPS para manter taxa natural
                // Para 60 FPS: ~16.67ms por frame, mas processamos 2 frames, então ~8ms
                int64_t frameTimeUs = 1000000LL / static_cast<int64_t>(m_fps); // Tempo por frame em microssegundos
                int64_t delayUs = frameTimeUs / 2;                             // Delay proporcional (já que processamos 2 frames)
                std::this_thread::sleep_for(std::chrono::microseconds(delayUs));
            }
        }
        else
        {
            // Se não processamos nada, verificar se há dados pendentes antes de dormir
            bool hasPendingData = (videoBufferSize > 0 || audioBufferSize > 0);

            if (!hasPendingData)
            {
                // Só dormir se realmente não há dados para processar
                std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 1ms quando não há dados
            }
            else
            {
                // Há dados mas não processamos (possivelmente aguardando sincronização)
                // Delay mínimo para não consumir CPU mas continuar tentando
                std::this_thread::sleep_for(std::chrono::microseconds(500)); // 500µs quando há dados mas não processou
            }
        }
    }

    return;
}

void HTTPTSStreamer::rawEncodingThread()
{
    // Mirror of encodingThread() but driving the /raw pipeline. Lives as a
    // near-duplicate for Phase 2 of #47; refactoring into a shared loop body
    // is intentionally deferred until the dual-output story is stable. Skips
    // doing any work when no /raw client is connected — Application.cpp gates
    // pushRawFrame via hasRawClients(), so under typical use this thread idles.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int cleanupCounter = 0;
    const int CLEANUP_INTERVAL = 10;

    // Rolling 1 s telemetry — surfaces whether the raw encoder is keeping
    // up with the push rate, and how often calculateSyncZone refuses to
    // hand frames over (which would starve the encoder even when CPU is
    // idle).
    auto statStart = std::chrono::steady_clock::now();
    uint32_t statVideoEncoded   = 0;
    uint32_t statAudioEncoded   = 0;
    uint32_t statIterations     = 0;
    size_t   statMaxQueueDepth  = 0;

    while (m_running)
    {
        if (m_stopRequest) break;

        bool processedAny = false;
        ++statIterations;

        cleanupCounter++;
        if (cleanupCounter >= CLEANUP_INTERVAL)
        {
            m_rawStreamSynchronizer.cleanupOldData();
            cleanupCounter = 0;
        }

        // Drain audio independently — same reasoning as encodingThread.
        {
            auto pendingAudio = m_rawStreamSynchronizer.getAllUnprocessedAudio();
            for (const auto &chunk : pendingAudio)
            {
                if (m_stopRequest) break;
                if (chunk.processed || !chunk.samples || chunk.sampleCount == 0) continue;

                std::vector<MediaEncoder::EncodedPacket> aPackets;
                if (m_rawMediaEncoder.encodeAudio(chunk.samples->data(), chunk.sampleCount,
                                                  chunk.captureTimestampUs, aPackets))
                {
                    for (const auto &p : aPackets)
                    {
                        m_rawMediaMuxer.muxPacket(p);
                    }
                    processedAny = true;
                    ++statAudioEncoded;
                }
                m_rawStreamSynchronizer.markAudioChunkProcessedByTimestamp(chunk.captureTimestampUs);
            }
        }

        size_t videoBufferSize = m_rawStreamSynchronizer.getVideoBufferSize();
        size_t audioBufferSize = m_rawStreamSynchronizer.getAudioBufferSize();
        if (videoBufferSize > statMaxQueueDepth) statMaxQueueDepth = videoBufferSize;
        bool hasBacklog = (videoBufferSize > 5 || audioBufferSize > 10);

        // Drain video independently of the audio sync zone, mirroring
        // the audio-drain path above. The /raw output is MPEG-TS, and
        // av_interleaved_write_frame already orders packets across
        // streams by DTS — we don't need calculateSyncZone to gate the
        // per-stream encoding. Gating on it produced "video=0/s
        // audio=0/s iters=1801 syncInvalid=1801" stalls whenever audio
        // capture jittered briefly and the audio/video buffers fell out
        // of the 50 ms overlap tolerance, even though the video queue
        // had frames ready to encode.
        {
            auto pendingVideo = m_rawStreamSynchronizer.getAllUnprocessedVideo();
            size_t framesProcessed = 0;
            size_t MAX_FRAMES_PER_ITERATION = hasBacklog ? 30 : 2;

            for (const auto &frame : pendingVideo)
            {
                if (m_stopRequest) break;
                if (framesProcessed >= MAX_FRAMES_PER_ITERATION) break;
                if (frame.processed || !frame.data || frame.width == 0 || frame.height == 0) continue;

                std::vector<MediaEncoder::EncodedPacket> packets;
                if (m_rawMediaEncoder.encodeVideo(frame.data->data(), frame.width, frame.height,
                                                 frame.captureTimestampUs, packets))
                {
                    for (const auto &packet : packets)
                    {
                        m_rawMediaMuxer.muxPacket(packet);
                    }
                    processedAny = true;
                    framesProcessed++;
                    ++statVideoEncoded;
                }
                m_rawStreamSynchronizer.markVideoFrameProcessedByTimestamp(frame.captureTimestampUs);
            }

            {
                std::lock_guard<std::mutex> lock(m_rawHeaderMutex);
                if (!m_rawHeaderWritten && m_rawMediaMuxer.isHeaderWritten())
                {
                    m_rawFormatHeader = m_rawMediaMuxer.getFormatHeader();
                    m_rawHeaderWritten = true;
                }
            }
        }

        // Emit telemetry once per second.
        auto nowTs = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(nowTs - statStart).count() >= 1)
        {
            LOG_DEBUG("/raw encoder: video=" + std::to_string(statVideoEncoded) +
                      "/s audio=" + std::to_string(statAudioEncoded) +
                      "/s iters=" + std::to_string(statIterations) +
                      " maxVidQueue=" + std::to_string(statMaxQueueDepth));
            statVideoEncoded = statAudioEncoded = statIterations = 0;
            statMaxQueueDepth = 0;
            statStart = nowTs;
        }

        if (processedAny)
        {
            if (hasBacklog)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            else
            {
                int64_t frameTimeUs = 1000000LL / static_cast<int64_t>(m_fps);
                std::this_thread::sleep_for(std::chrono::microseconds(frameTimeUs / 2));
            }
        }
        else
        {
            bool hasPendingData = (videoBufferSize > 0 || audioBufferSize > 0);
            if (!hasPendingData)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        }
    }
}
