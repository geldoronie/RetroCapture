#include "HTTPTSStreamer.h"
#include "../utils/HttpAuth.h"
#include "../utils/Logger.h"
#include "../utils/Paths.h"

#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include <cstring>
#include <cstdio>
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

// Enable aggressive TCP keep-alive on a client socket so a viewer that
// vanishes WITHOUT a clean FIN (network drop, a remote client whose
// TLS read errored and was reaped over the internet) is detected by
// the kernel in ~25 s instead of the default ~2 h. The disconnect
// monitor loop's blocking recv() then returns and the client is
// reaped, so the viewer count converges to reality.
//
// Without this, half-open connections from failed/repeated connects
// pile up in m_clientSockets / m_rawClientSockets and inflate the
// reported viewer count (the symptom: "counter already >6 even
// without completing a connection"). #92.
static void enableClientKeepalive(int fd)
{
    if (fd < 0) return;
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
#  if defined(PLATFORM_LINUX)
    int idle  = 10; // begin probing after 10 s idle
    int intvl = 5;  // probe every 5 s
    int cnt   = 3;  // 3 missed probes → dead (~25 s total)
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,  &idle,  sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,   sizeof(cnt));
#  elif defined(PLATFORM_MACOS)
    int idle = 10; // macOS: seconds before first probe
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
#  endif
#else
    // Windows: SO_KEEPALIVE via the winsock fd (host build links winsock).
    DWORD on = 1;
    setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_KEEPALIVE,
               reinterpret_cast<const char *>(&on), sizeof(on));
#endif
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
        LOG_WARN("HTTPS enabled but certificates not configured. Falling back to HTTP.");
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
        LOG_WARN("HTTP server already active");
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
            LOG_WARN("HTTPS enabled but certificates not configured. Falling back to HTTP.");
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
    // Combine /stream subscribers (web portal viewers consuming
    // MPEG-TS) and /raw subscribers (remote RetroCapture desktop
    // clients consuming the shader-bypassed feed). Both are "people
    // watching the host's broadcast" — surfacing only one of them
    // makes the count under-report whenever the audience splits
    // between the portal and remote-client viewers (#68).
    return m_clientCount.load() + m_rawClientCount.load();
}

void HTTPTSStreamer::cleanup()
{
    stop();
}

bool HTTPTSStreamer::readClientRequest(int clientFd, std::string &request)
{
// Configurar socket para baixa latência
#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS)
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
        return false;
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
        return false;
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
    return true;
}


void HTTPTSStreamer::serveRawClient(int clientFd, const std::string &request)
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

    enableClientKeepalive(clientFd);
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


void HTTPTSStreamer::serveStreamClient(int clientFd)
{
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
    enableClientKeepalive(clientFd);
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


void HTTPTSStreamer::handleClient(int clientFd)
{
    std::string request;
    if (!readClientRequest(clientFd, request))
        return;
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
        serveRawClient(clientFd, request);
        return;
    }

    serveStreamClient(clientFd);
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
                // Slow /raw receiver (remote hop, congested link). Closing
                // it here forced the client into a full messy reconnect —
                // re-probe, deferred audio sink, multi-second A/V desync,
                // audio dropout (#93). The /raw consumer is an FFmpeg
                // demuxer that resyncs at the next in-band keyframe/PAT, so
                // instead of tearing down we DROP the queued backlog and
                // keep the connection. We clear the whole tail (rather than
                // a partial trim) so what we resume sending stays 188-byte
                // TS-packet aligned; the client sees a brief glitch, not a
                // disconnect. /stream (browser mpegts.js) is left closing
                // because it cannot tolerate a mid-stream byte drop.
                if (p.pending() > kMaxClientBacklog)
                {
                    p.tail.clear();
                    p.tailOffset = 0;
                    uint64_t n = ++p.backlogDrops;
                    if (n == 1 || (n % 30) == 0)
                    {
                        LOG_WARN("/raw client fd=" + std::to_string(clientFd) +
                                 " send backlog exceeded cap — dropped backlog, keeping connection (total drops: " +
                                 std::to_string(n) + ")");
                    }
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
    m_streamSynchronizer.setName("stream");
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
    m_rawStreamSynchronizer.setName("raw");
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
            // #123 — per-stage averages (ms/frame) so we can see whether the
            // CPU swscale convert, the HW surface upload, or the codec is the
            // 60 fps bottleneck. avg = accumulated µs / frames encoded.
            auto st = m_rawMediaEncoder.fetchVideoStageTimings();
            std::string line = "/raw encoder: video=" + std::to_string(statVideoEncoded) +
                               "/s audio=" + std::to_string(statAudioEncoded) +
                               "/s iters=" + std::to_string(statIterations) +
                               " maxVidQueue=" + std::to_string(statMaxQueueDepth);
            if (st.frames > 0)
            {
                double convMs = (st.convertUs / 1000.0) / st.frames;
                double upMs   = (st.uploadUs  / 1000.0) / st.frames;
                double encMs  = (st.encodeUs  / 1000.0) / st.frames;
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              " stages(ms/f): convert=%.2f upload=%.2f encode=%.2f total=%.2f",
                              convMs, upMs, encMs, convMs + upMs + encMs);
                line += buf;
                // Active /raw client: surface at INFO so the bottleneck is
                // visible without enabling debug logging (#123 measurement).
                LOG_INFO(line);
            }
            else
            {
                LOG_DEBUG(line);
            }
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
