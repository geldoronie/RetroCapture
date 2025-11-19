#include "HTTPTSStreamer.h"
#include "../utils/Logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <time.h>

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
    m_cleanedUp = false;
}

HTTPTSStreamer::~HTTPTSStreamer()
{
    cleanup();
}

bool HTTPTSStreamer::initialize(uint16_t port, uint32_t width, uint32_t height, uint32_t fps)
{
    m_port = port;
    m_width = width;
    m_height = height;
    m_fps = fps;

    // Limpar buffers
    {
        std::lock_guard<std::mutex> videoLock(m_videoMutex);
        while (!m_videoQueue.empty())
        {
            m_videoQueue.pop();
        }
    }
    {
        std::lock_guard<std::mutex> audioLock(m_audioMutex);
        m_audioBuffer.clear();
    }

    LOG_INFO("HTTP MPEG-TS Streamer inicializado (OBS style): " + std::to_string(width) + "x" + std::to_string(height) +
             " @ " + std::to_string(fps) + "fps, porta " + std::to_string(port));
    return true;
}

void HTTPTSStreamer::setAudioFormat(uint32_t sampleRate, uint32_t channels)
{
    m_sampleRate = sampleRate;
    m_channels = channels;

    // Limpar buffer de áudio
    {
        std::lock_guard<std::mutex> audioLock(m_audioMutex);
        m_audioBuffer.clear();
    }

    LOG_INFO("Formato de áudio configurado: " + std::to_string(sampleRate) + "Hz, " + std::to_string(channels) + " canais");
}

void HTTPTSStreamer::setVideoCodec(const std::string &codecName)
{
    m_videoCodecName = codecName;
    LOG_INFO("Codec de vídeo configurado: " + codecName);
}

void HTTPTSStreamer::setAudioCodec(const std::string &codecName)
{
    m_audioCodecName = codecName;
    LOG_INFO("Codec de áudio configurado: " + codecName);
}

bool HTTPTSStreamer::pushFrame(const uint8_t *data, uint32_t width, uint32_t height)
{
    if (!data || !m_active)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_videoMutex);

    // Se a fila estiver muito cheia, logar mas NÃO descartar
    // O encodingThread vai processar e liberar espaço
    if (m_videoQueue.size() >= MAX_VIDEO_QUEUE_SIZE)
    {
        // Log apenas ocasionalmente para não spammar
        static int logCounter = 0;
        if (logCounter++ % 60 == 0)
        {
            LOG_INFO("Fila de vídeo cheia (" + std::to_string(m_videoQueue.size()) + " frames), aguardando processamento...");
        }
        // NÃO descartar - aguardar que o encodingThread processe
        // Se realmente não couber, o frame será perdido, mas isso é melhor que descartar frames antigos
        if (m_videoQueue.size() >= MAX_VIDEO_QUEUE_SIZE * 2)
        {
            // Apenas em caso extremo (fila 2x maior que o máximo), descartar o mais antigo
            m_videoQueue.pop();
        }
    }

    // Adicionar frame
    VideoFrame frame;
    size_t frameSize = width * height * 3;
    frame.data.resize(frameSize);
    memcpy(frame.data.data(), data, frameSize);
    frame.width = width;
    frame.height = height;
    m_videoQueue.push(std::move(frame));

    return true;
}

bool HTTPTSStreamer::pushAudio(const int16_t *samples, size_t sampleCount)
{
    if (!samples || !m_active || sampleCount == 0)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_audioMutex);

    // Limitar tamanho do buffer
    size_t maxSamples = MAX_AUDIO_BUFFER_SAMPLES;
    if (m_audioBuffer.size() + sampleCount > maxSamples)
    {
        // Descartar samples mais antigos
        size_t toDiscard = (m_audioBuffer.size() + sampleCount) - maxSamples;
        if (toDiscard >= m_audioBuffer.size())
        {
            m_audioBuffer.clear();
        }
        else
        {
            m_audioBuffer.erase(m_audioBuffer.begin(), m_audioBuffer.begin() + toDiscard);
        }
    }

    // Adicionar samples
    m_audioBuffer.insert(m_audioBuffer.end(), samples, samples + sampleCount);

    return true;
}

bool HTTPTSStreamer::start()
{
    if (m_active)
    {
        return true;
    }

    // Limpar buffers (OBS style - independentes)
    {
        std::lock_guard<std::mutex> videoLock(m_videoMutex);
        while (!m_videoQueue.empty())
        {
            m_videoQueue.pop();
        }
    }
    {
        std::lock_guard<std::mutex> audioLock(m_audioMutex);
        m_audioBuffer.clear();
    }

    // Create server socket
    m_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSocket < 0)
    {
        LOG_ERROR("Falha ao criar socket HTTP");
        return false;
    }

    // Set socket options
    int opt = 1;
    setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind socket
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_port);

    if (bind(m_serverSocket, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        LOG_ERROR("Falha ao fazer bind na porta " + std::to_string(m_port));
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    // Listen
    if (listen(m_serverSocket, 5) < 0)
    {
        LOG_ERROR("Falha ao fazer listen");
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    // Initialize FFmpeg
    if (!initializeFFmpeg())
    {
        LOG_ERROR("Falha ao inicializar FFmpeg");
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    m_running = true;
    m_active = true;
    m_serverThread = std::thread(&HTTPTSStreamer::serverThread, this);
    m_encodingThread = std::thread(&HTTPTSStreamer::encodingThread, this);

    LOG_INFO("HTTP MPEG-TS Streamer iniciado na porta " + std::to_string(m_port));
    return true;
}

void HTTPTSStreamer::stop()
{
    if (!m_active)
    {
        return;
    }

    m_running = false;
    m_active = false;

    // Close server socket to wake up accept()
    if (m_serverSocket >= 0)
    {
        close(m_serverSocket);
        m_serverSocket = -1;
    }

    if (m_serverThread.joinable())
    {
        m_serverThread.join();
    }

    if (m_encodingThread.joinable())
    {
        m_encodingThread.join();
    }

    // Flush codecs antes de limpar
    flushCodecs();

    cleanupFFmpeg();

    LOG_INFO("HTTP MPEG-TS Streamer parado");
}

bool HTTPTSStreamer::isActive() const
{
    return m_active;
}

std::string HTTPTSStreamer::getStreamUrl() const
{
    return "http://localhost:" + std::to_string(m_port) + "/stream";
}

uint32_t HTTPTSStreamer::getClientCount() const
{
    return m_clientCount.load();
}

void HTTPTSStreamer::cleanup()
{
    stop();
}

void HTTPTSStreamer::serverThread()
{
    while (m_running)
    {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);

        int clientFd = accept(m_serverSocket, (struct sockaddr *)&clientAddr, &clientLen);
        if (clientFd < 0)
        {
            if (m_running)
            {
                LOG_ERROR("Falha ao aceitar conexão HTTP");
            }
            break;
        }

        // Handle client in separate thread
        std::thread clientThread(&HTTPTSStreamer::handleClient, this, clientFd);
        clientThread.detach();
    }
}

void HTTPTSStreamer::handleClient(int clientFd)
{
    // Configurar socket para baixa latência
    int flag = 1;
    setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)); // Desabilitar Nagle's algorithm

    // Reduzir buffer de envio para forçar envio imediato
    int sendBufSize = 8192; // 8KB buffer pequeno
    setsockopt(clientFd, SOL_SOCKET, SO_SNDBUF, &sendBufSize, sizeof(sendBufSize));

    // Read HTTP request FIRST, before incrementing counter
    char buffer[4096];
    ssize_t bytesRead = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
    if (bytesRead <= 0)
    {
        close(clientFd);
        return;
    }

    buffer[bytesRead] = '\0';

    // Check if it's a GET request for /stream
    std::string request(buffer);
    bool isStreamRequest = (request.find("GET /stream") != std::string::npos);

    // Only accept /stream requests, reject everything else (favicon, etc.)
    if (!isStreamRequest)
    {
        // Send 404
        const char *response = "HTTP/1.1 404 Not Found\r\n\r\n";
        send(clientFd, response, strlen(response), 0);
        close(clientFd);
        return;
    }

    // Add client to list
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        m_clientSockets.push_back(clientFd);
        uint32_t countAfter = ++m_clientCount;
        LOG_INFO("Cliente HTTP MPEG-TS conectado (total: " + std::to_string(countAfter) + ")");
    }

    // Send HTTP headers for MPEG-TS stream
    std::ostringstream headers;
    headers << "HTTP/1.1 200 OK\r\n";
    headers << "Content-Type: video/mp2t\r\n";
    headers << "Connection: keep-alive\r\n";
    headers << "Cache-Control: no-cache\r\n";
    headers << "Pragma: no-cache\r\n";
    headers << "\r\n";

    std::string headerStr = headers.str();
    ssize_t sent = send(clientFd, headerStr.c_str(), headerStr.length(), MSG_NOSIGNAL);
    if (sent < 0)
    {
        // Client disconnected or error
        int err = errno;
        {
            std::lock_guard<std::mutex> lock(m_outputMutex);
            auto it = std::find(m_clientSockets.begin(), m_clientSockets.end(), clientFd);
            if (it != m_clientSockets.end())
            {
                m_clientSockets.erase(it);
                uint32_t countAfter = --m_clientCount;
                LOG_INFO("Cliente HTTP MPEG-TS desconectado (erro ao enviar headers: " + std::to_string(err) + ", total: " + std::to_string(countAfter) + ")");
            }
        }
        close(clientFd);
        return;
    }

    // Verificar se todos os bytes foram enviados
    if (static_cast<size_t>(sent) < headerStr.length())
    {
        // Tentar enviar o restante
        size_t remaining = headerStr.length() - sent;
        const char *remainingData = headerStr.c_str() + sent;
        ssize_t retrySent = send(clientFd, remainingData, remaining, MSG_NOSIGNAL);
        if (retrySent < 0 || static_cast<size_t>(retrySent) < remaining)
        {
            int err = errno;
            {
                std::lock_guard<std::mutex> lock(m_outputMutex);
                auto it = std::find(m_clientSockets.begin(), m_clientSockets.end(), clientFd);
                if (it != m_clientSockets.end())
                {
                    m_clientSockets.erase(it);
                    uint32_t countAfter = --m_clientCount;
                    LOG_INFO("Cliente HTTP MPEG-TS desconectado (erro ao enviar headers completo: " + std::to_string(err) + ", total: " + std::to_string(countAfter) + ")");
                }
            }
            close(clientFd);
            return;
        }
    }

    // Stream data (data is sent via writeToClients callback from encoding thread)
    // Just wait here until client disconnects
    while (m_running)
    {
        char dummy;
        ssize_t result = recv(clientFd, &dummy, 1, MSG_PEEK);
        if (result <= 0)
        {
            break; // Client disconnected
        }
        // Sleep briefly to avoid busy-waiting
        usleep(10000); // 10ms
    }

    // Cliente desconectou - remover da lista e decrementar contador
    close(clientFd);
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        // Verificar se ainda está na lista antes de remover
        auto it = std::find(m_clientSockets.begin(), m_clientSockets.end(), clientFd);
        if (it != m_clientSockets.end())
        {
            m_clientSockets.erase(it);
            uint32_t countAfter = --m_clientCount;
            LOG_INFO("Cliente HTTP MPEG-TS desconectado (total: " + std::to_string(countAfter) + ")");
        }
        else
        {
            // Já foi removido por writeToClients, não decrementar novamente
            LOG_INFO("Cliente HTTP MPEG-TS desconectado (já removido da lista)");
        }
    }
}

int HTTPTSStreamer::writeToClients(const uint8_t *buf, int buf_size)
{
    if (!buf || buf_size <= 0)
    {
        return buf_size; // Dados inválidos, mas retornar sucesso para não quebrar o FFmpeg
    }

    std::lock_guard<std::mutex> lock(m_outputMutex);

    if (m_clientSockets.empty())
    {
        return buf_size; // Nenhum cliente, mas retornar sucesso
    }

    // Enviar para todos os clientes conectados
    // Remover clientes que falharam ao enviar
    auto it = m_clientSockets.begin();
    while (it != m_clientSockets.end())
    {
        int clientFd = *it;

        // Enviar dados em chunks se necessário (alguns sistemas têm limite de tamanho)
        size_t totalSent = 0;
        bool error = false;

        while (totalSent < static_cast<size_t>(buf_size) && !error)
        {
            size_t toSend = static_cast<size_t>(buf_size) - totalSent;
            // Limitar chunk size para evitar problemas
            if (toSend > 65536)
            {
                toSend = 65536;
            }

            ssize_t sent = send(clientFd, buf + totalSent, toSend, MSG_NOSIGNAL);
            if (sent < 0)
            {
                int err = errno;
                // EPIPE e ECONNRESET são normais quando cliente desconecta
                // EAGAIN/EWOULDBLOCK significa que o socket não está pronto, mas não é erro fatal
                if (err != EAGAIN && err != EWOULDBLOCK)
                {
                    error = true;
                    close(clientFd);
                    it = m_clientSockets.erase(it);
                    uint32_t countAfter = --m_clientCount;
                    LOG_INFO("Cliente HTTP MPEG-TS desconectado (erro ao enviar dados: " + std::to_string(err) + ", total: " + std::to_string(countAfter) + ")");
                    break;
                }
                else
                {
                    // Socket não está pronto, tentar novamente em breve
                    // Mas não remover o cliente
                    usleep(1000); // 1ms
                    continue;
                }
            }
            else if (sent == 0)
            {
                // Cliente fechou a conexão
                error = true;
                close(clientFd);
                it = m_clientSockets.erase(it);
                uint32_t countAfter = --m_clientCount;
                LOG_INFO("Cliente HTTP MPEG-TS desconectado (conexão fechada, total: " + std::to_string(countAfter) + ")");
                break;
            }
            else
            {
                totalSent += sent;
            }
        }

        if (!error)
        {
            ++it;
        }
    }

    return buf_size; // Sempre retornar buf_size para o FFmpeg
}

bool HTTPTSStreamer::initializeFFmpeg()
{
    // Se já está inicializado, limpar primeiro
    if (m_ffmpeg.formatCtx != nullptr)
    {
        cleanupFFmpeg();
    }

    AVFormatContext *formatCtx = nullptr;
    AVCodecContext *videoCodecCtx = nullptr;
    AVCodecContext *audioCodecCtx = nullptr;
    AVCodec *videoCodec = nullptr;
    AVCodec *audioCodec = nullptr;
    AVStream *videoStream = nullptr;
    AVStream *audioStream = nullptr;

    // Allocate format context
    formatCtx = avformat_alloc_context();
    if (!formatCtx)
    {
        LOG_ERROR("Falha ao alocar format context");
        return false;
    }

    // Allocate format buffer (FFmpeg will manage it)
    m_ffmpeg.formatBufferSize = 4 * 1024 * 1024; // 4MB buffer - aumentado para melhor fluidez
    m_ffmpeg.formatBuffer = static_cast<uint8_t *>(av_malloc(m_ffmpeg.formatBufferSize));
    if (!m_ffmpeg.formatBuffer)
    {
        LOG_ERROR("Falha ao alocar format buffer");
        avformat_free_context(formatCtx);
        return false;
    }

    // Create AVIO context for writing to our callback
    AVIOContext *avioCtx = avio_alloc_context(
        m_ffmpeg.formatBuffer, static_cast<int>(m_ffmpeg.formatBufferSize),
        1,             // write_flag
        this,          // opaque
        nullptr,       // read_packet (not used)
        writeCallback, // write_packet
        nullptr);      // seek (not used)

    if (!avioCtx)
    {
        LOG_ERROR("Falha ao criar AVIO context");
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    formatCtx->pb = avioCtx;
    formatCtx->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_NOFILE;

    // Find MPEG-TS muxer
    const AVOutputFormat *fmt = av_guess_format("mpegts", nullptr, nullptr);
    if (!fmt)
    {
        LOG_ERROR("MPEG-TS muxer não encontrado");
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    formatCtx->oformat = fmt;
    // filename não existe mais nas versões recentes do FFmpeg, usar url
    formatCtx->url = av_strdup("dummy.mpegts");

    // Find video codec
    AVCodecID videoCodecID = AV_CODEC_ID_H264;
    if (m_videoCodecName == "h265" || m_videoCodecName == "hevc")
    {
        videoCodecID = AV_CODEC_ID_H265;
    }
    else if (m_videoCodecName == "vp8")
    {
        videoCodecID = AV_CODEC_ID_VP8;
    }
    else if (m_videoCodecName == "vp9")
    {
        videoCodecID = AV_CODEC_ID_VP9;
    }

    videoCodec = const_cast<AVCodec *>(avcodec_find_encoder(videoCodecID));
    if (!videoCodec)
    {
        LOG_ERROR("Codec de vídeo não encontrado: " + m_videoCodecName);
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    videoStream = avformat_new_stream(formatCtx, videoCodec);
    if (!videoStream)
    {
        LOG_ERROR("Falha ao criar video stream");
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    videoCodecCtx = avcodec_alloc_context3(videoCodec);
    if (!videoCodecCtx)
    {
        LOG_ERROR("Falha ao alocar video codec context");
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    // Configure video codec
    videoCodecCtx->codec_id = videoCodecID;
    videoCodecCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    videoCodecCtx->width = static_cast<int>(m_width);
    videoCodecCtx->height = static_cast<int>(m_height);
    videoCodecCtx->time_base = {1, static_cast<int>(m_fps)};
    videoCodecCtx->framerate = {static_cast<int>(m_fps), 1};
    videoCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    videoCodecCtx->bit_rate = m_videoBitrate;
    // GOP size baseado no FPS configurado (0.5 segundos = m_fps / 2)
    // Garantir mínimo de 1 frame para evitar divisão por zero
    int gopSize = std::max(1, static_cast<int>(m_fps / 2));
    videoCodecCtx->gop_size = gopSize;
    videoCodecCtx->max_b_frames = 0; // Sem B-frames para evitar problemas de ordem

    // H.264 specific options
    if (videoCodecID == AV_CODEC_ID_H264)
    {
        av_opt_set(videoCodecCtx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(videoCodecCtx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(videoCodecCtx->priv_data, "profile", "baseline", 0);
        // keyint_min baseado no FPS configurado (0.5 segundos = m_fps / 2)
        int keyintMin = std::max(1, static_cast<int>(m_fps / 2));
        av_opt_set_int(videoCodecCtx->priv_data, "keyint_min", keyintMin, 0);
    }

    if (avcodec_open2(videoCodecCtx, videoCodec, nullptr) < 0)
    {
        LOG_ERROR("Falha ao abrir video codec");
        avcodec_free_context(&videoCodecCtx);
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    // Copy codec parameters to stream
    if (avcodec_parameters_from_context(videoStream->codecpar, videoCodecCtx) < 0)
    {
        LOG_ERROR("Falha ao copiar parâmetros do codec de vídeo");
        avcodec_free_context(&videoCodecCtx);
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    // Find audio codec
    AVCodecID audioCodecID = AV_CODEC_ID_AAC;
    if (m_audioCodecName == "mp3")
    {
        audioCodecID = AV_CODEC_ID_MP3;
    }
    else if (m_audioCodecName == "opus")
    {
        audioCodecID = AV_CODEC_ID_OPUS;
    }

    audioCodec = const_cast<AVCodec *>(avcodec_find_encoder(audioCodecID));
    if (!audioCodec)
    {
        LOG_ERROR("Codec de áudio não encontrado: " + m_audioCodecName);
        avcodec_free_context(&videoCodecCtx);
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    audioStream = avformat_new_stream(formatCtx, audioCodec);
    if (!audioStream)
    {
        LOG_ERROR("Falha ao criar audio stream");
        avcodec_free_context(&videoCodecCtx);
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    audioCodecCtx = avcodec_alloc_context3(audioCodec);
    if (!audioCodecCtx)
    {
        LOG_ERROR("Falha ao alocar audio codec context");
        avcodec_free_context(&videoCodecCtx);
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    // Configure audio codec
    audioCodecCtx->codec_id = audioCodecID;
    audioCodecCtx->codec_type = AVMEDIA_TYPE_AUDIO;
    audioCodecCtx->sample_rate = static_cast<int>(m_sampleRate);

    // Configurar channel layout corretamente
    if (m_channels == 1)
    {
        av_channel_layout_default(&audioCodecCtx->ch_layout, 1);
    }
    else
    {
        av_channel_layout_default(&audioCodecCtx->ch_layout, 2);
    }

    audioCodecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP; // AAC requires float planar
    audioCodecCtx->bit_rate = m_audioBitrate;
    audioCodecCtx->time_base = {1, static_cast<int>(m_sampleRate)};

    // AAC specific: frame size
    if (audioCodecID == AV_CODEC_ID_AAC)
    {
        audioCodecCtx->frame_size = 1024;
        m_ffmpeg.audioFrameSize = 1024;
    }
    else
    {
        audioCodecCtx->frame_size = 1152; // MP3
        m_ffmpeg.audioFrameSize = 1152;
    }

    if (avcodec_open2(audioCodecCtx, audioCodec, nullptr) < 0)
    {
        LOG_ERROR("Falha ao abrir audio codec");
        avcodec_free_context(&audioCodecCtx);
        avcodec_free_context(&videoCodecCtx);
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    // Copy codec parameters to stream
    if (avcodec_parameters_from_context(audioStream->codecpar, audioCodecCtx) < 0)
    {
        LOG_WARN("avcodec_parameters_from_context falhou, copiando manualmente");
        // Copiar manualmente se falhar
        audioStream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        audioStream->codecpar->codec_id = audioCodecID;
        audioStream->codecpar->sample_rate = static_cast<int>(m_sampleRate);
        if (m_channels == 1)
        {
            av_channel_layout_default(&audioStream->codecpar->ch_layout, 1);
        }
        else
        {
            av_channel_layout_default(&audioStream->codecpar->ch_layout, 2);
        }
        audioStream->codecpar->format = audioCodecCtx->sample_fmt;
        audioStream->codecpar->bit_rate = m_audioBitrate;
    }

    // IMPORTANTE: Garantir que os parâmetros foram copiados corretamente
    // Se ainda estiverem zerados, copiar novamente
    if (audioStream->codecpar->ch_layout.nb_channels == 0)
    {
        LOG_WARN("Channel layout ainda zerado após copiar parâmetros, configurando novamente");
        av_channel_layout_uninit(&audioStream->codecpar->ch_layout);
        if (m_channels == 1)
        {
            av_channel_layout_default(&audioStream->codecpar->ch_layout, 1);
        }
        else
        {
            av_channel_layout_default(&audioStream->codecpar->ch_layout, 2);
        }
    }

    if (audioStream->codecpar->sample_rate == 0)
    {
        LOG_WARN("Sample rate ainda zerado após copiar parâmetros, configurando novamente");
        audioStream->codecpar->sample_rate = static_cast<int>(m_sampleRate);
    }

    if (audioStream->codecpar->format == AV_SAMPLE_FMT_NONE)
    {
        LOG_WARN("Sample format ainda não configurado, configurando");
        audioStream->codecpar->format = audioCodecCtx->sample_fmt;
    }

    // IMPORTANTE: Configurar time_base do stream explicitamente
    audioStream->time_base = {1, static_cast<int>(m_sampleRate)};

    // Verificar se todos os parâmetros estão corretos antes de escrever header
    if (audioStream->codecpar->ch_layout.nb_channels == 0)
    {
        LOG_ERROR("Audio stream não tem canais configurados!");
        avcodec_free_context(&audioCodecCtx);
        avcodec_free_context(&videoCodecCtx);
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    if (audioStream->codecpar->sample_rate == 0)
    {
        LOG_ERROR("Audio stream não tem sample rate configurado!");
        avcodec_free_context(&audioCodecCtx);
        avcodec_free_context(&videoCodecCtx);
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    // Log dos parâmetros antes de escrever header
    LOG_INFO("Audio stream configurado: index=" + std::to_string(audioStream->index) +
             ", channels=" + std::to_string(audioStream->codecpar->ch_layout.nb_channels) +
             ", sample_rate=" + std::to_string(audioStream->codecpar->sample_rate) +
             ", codec_id=" + std::to_string(audioStream->codecpar->codec_id));

    // Write header
    if (avformat_write_header(formatCtx, nullptr) < 0)
    {
        LOG_ERROR("Falha ao escrever header MPEG-TS");
        avcodec_free_context(&audioCodecCtx);
        avcodec_free_context(&videoCodecCtx);
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    // Verificar se o stream ainda está presente após escrever header
    LOG_INFO("Total streams após escrever header: " + std::to_string(formatCtx->nb_streams));
    for (unsigned int i = 0; i < formatCtx->nb_streams; i++)
    {
        AVStream *stream = formatCtx->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            LOG_INFO("Stream " + std::to_string(i) + ": VÍDEO, codec=" +
                     std::to_string(stream->codecpar->codec_id) +
                     ", " + std::to_string(stream->codecpar->width) + "x" +
                     std::to_string(stream->codecpar->height));
        }
        else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            LOG_INFO("Stream " + std::to_string(i) + ": ÁUDIO, codec=" +
                     std::to_string(stream->codecpar->codec_id) +
                     ", channels=" + std::to_string(stream->codecpar->ch_layout.nb_channels) +
                     ", sample_rate=" + std::to_string(stream->codecpar->sample_rate));
        }
    }

    if (formatCtx->nb_streams < 2)
    {
        LOG_ERROR("Stream de áudio não está presente após escrever header! Total streams: " +
                  std::to_string(formatCtx->nb_streams));
        return false;
    }

    // Allocate frames
    AVFrame *videoFrame = av_frame_alloc();
    AVFrame *audioFrame = av_frame_alloc();
    if (!videoFrame || !audioFrame)
    {
        LOG_ERROR("Falha ao alocar frames");
        if (videoFrame)
            av_frame_free(&videoFrame);
        if (audioFrame)
            av_frame_free(&audioFrame);
        avcodec_free_context(&audioCodecCtx);
        avcodec_free_context(&videoCodecCtx);
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    videoFrame->format = videoCodecCtx->pix_fmt;
    videoFrame->width = videoCodecCtx->width;
    videoFrame->height = videoCodecCtx->height;
    if (av_frame_get_buffer(videoFrame, 0) < 0)
    {
        LOG_ERROR("Falha ao alocar buffer do video frame");
        av_frame_free(&videoFrame);
        av_frame_free(&audioFrame);
        avcodec_free_context(&audioCodecCtx);
        avcodec_free_context(&videoCodecCtx);
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    audioFrame->format = audioCodecCtx->sample_fmt;
    av_channel_layout_copy(&audioFrame->ch_layout, &audioCodecCtx->ch_layout);
    audioFrame->sample_rate = audioCodecCtx->sample_rate;
    audioFrame->nb_samples = audioCodecCtx->frame_size;
    if (av_frame_get_buffer(audioFrame, 0) < 0)
    {
        LOG_ERROR("Falha ao alocar buffer do audio frame");
        av_frame_free(&videoFrame);
        av_frame_free(&audioFrame);
        avcodec_free_context(&audioCodecCtx);
        avcodec_free_context(&videoCodecCtx);
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    // Create SWS context for RGB to YUV conversion
    SwsContext *swsCtx = sws_getContext(
        static_cast<int>(m_width), static_cast<int>(m_height), AV_PIX_FMT_RGB24,
        static_cast<int>(m_width), static_cast<int>(m_height), AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!swsCtx)
    {
        LOG_ERROR("Falha ao criar SWS context");
        av_frame_free(&videoFrame);
        av_frame_free(&audioFrame);
        avcodec_free_context(&audioCodecCtx);
        avcodec_free_context(&videoCodecCtx);
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    // Create SWR context for audio resampling
    SwrContext *swrCtx = swr_alloc();
    if (!swrCtx)
    {
        LOG_ERROR("Falha ao alocar SWR context");
        sws_freeContext(swsCtx);
        av_frame_free(&videoFrame);
        av_frame_free(&audioFrame);
        avcodec_free_context(&audioCodecCtx);
        avcodec_free_context(&videoCodecCtx);
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    // Configure SWR for converting int16 PCM to float planar
    AVChannelLayout inChLayout, outChLayout;
    if (m_channels == 1)
    {
        av_channel_layout_default(&inChLayout, 1);
        av_channel_layout_default(&outChLayout, 1);
    }
    else
    {
        av_channel_layout_default(&inChLayout, 2);
        av_channel_layout_default(&outChLayout, 2);
    }

    av_opt_set_chlayout(swrCtx, "in_chlayout", &inChLayout, 0);
    av_opt_set_int(swrCtx, "in_sample_rate", static_cast<int>(m_sampleRate), 0);
    av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    av_opt_set_chlayout(swrCtx, "out_chlayout", &outChLayout, 0);
    av_opt_set_int(swrCtx, "out_sample_rate", static_cast<int>(m_sampleRate), 0);
    av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);

    if (swr_init(swrCtx) < 0)
    {
        LOG_ERROR("Falha ao inicializar SWR context");
        av_channel_layout_uninit(&inChLayout);
        av_channel_layout_uninit(&outChLayout);
        swr_free(&swrCtx);
        sws_freeContext(swsCtx);
        av_frame_free(&videoFrame);
        av_frame_free(&audioFrame);
        avcodec_free_context(&audioCodecCtx);
        avcodec_free_context(&videoCodecCtx);
        avio_context_free(&avioCtx);
        av_free(m_ffmpeg.formatBuffer);
        avformat_free_context(formatCtx);
        return false;
    }

    // Store pointers
    m_ffmpeg.formatCtx = formatCtx;
    m_ffmpeg.videoCodecCtx = videoCodecCtx;
    m_ffmpeg.audioCodecCtx = audioCodecCtx;
    m_ffmpeg.videoStream = videoStream;
    m_ffmpeg.audioStream = audioStream;
    m_ffmpeg.videoFrame = videoFrame;
    m_ffmpeg.audioFrame = audioFrame;
    m_ffmpeg.swsCtx = swsCtx;
    m_ffmpeg.swrCtx = swrCtx;

    // Cleanup temporary layouts after SWR init
    av_channel_layout_uninit(&inChLayout);
    av_channel_layout_uninit(&outChLayout);

    // Reset counters
    m_ffmpeg.videoPts = 0;
    m_ffmpeg.audioPts = 0;
    m_ffmpeg.videoFrameCount = 0;
    m_ffmpeg.audioSampleCount = 0;
    m_ffmpeg.lastVideoDts = -1;
    m_ffmpeg.lastAudioDts = -1;
    m_ffmpeg.forceKeyFrame = true; // Forçar keyframe no primeiro frame

    LOG_INFO("FFmpeg inicializado para MPEG-TS streaming");
    LOG_INFO("Audio codec: " + std::to_string(audioCodecCtx->ch_layout.nb_channels) + " channels, " +
             std::to_string(audioCodecCtx->sample_rate) + "Hz");
    return true;
}

void HTTPTSStreamer::cleanupFFmpeg()
{
    if (m_cleanedUp.load())
    {
        return; // Já foi limpo
    }

    AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_ffmpeg.formatCtx);
    AVCodecContext *videoCodecCtx = static_cast<AVCodecContext *>(m_ffmpeg.videoCodecCtx);
    AVCodecContext *audioCodecCtx = static_cast<AVCodecContext *>(m_ffmpeg.audioCodecCtx);
    AVFrame *videoFrame = static_cast<AVFrame *>(m_ffmpeg.videoFrame);
    AVFrame *audioFrame = static_cast<AVFrame *>(m_ffmpeg.audioFrame);
    SwsContext *swsCtx = static_cast<SwsContext *>(m_ffmpeg.swsCtx);
    SwrContext *swrCtx = static_cast<SwrContext *>(m_ffmpeg.swrCtx);

    if (swrCtx)
    {
        swr_free(&swrCtx);
        m_ffmpeg.swrCtx = nullptr;
    }

    if (swsCtx)
    {
        sws_freeContext(swsCtx);
        m_ffmpeg.swsCtx = nullptr;
    }

    if (audioFrame)
    {
        av_frame_free(&audioFrame);
        m_ffmpeg.audioFrame = nullptr;
    }

    if (videoFrame)
    {
        av_frame_free(&videoFrame);
        m_ffmpeg.videoFrame = nullptr;
    }

    if (audioCodecCtx)
    {
        avcodec_free_context(&audioCodecCtx);
        m_ffmpeg.audioCodecCtx = nullptr;
    }

    if (videoCodecCtx)
    {
        avcodec_free_context(&videoCodecCtx);
        m_ffmpeg.videoCodecCtx = nullptr;
    }

    if (formatCtx)
    {
        AVIOContext *avioCtx = formatCtx->pb;
        if (avioCtx)
        {
            // formatBuffer é gerenciado pelo AVIOContext, não precisamos liberar manualmente
            avio_context_free(&avioCtx);
        }
        if (formatCtx->url)
        {
            av_free(const_cast<char *>(formatCtx->url));
        }
        // Cleanup channel layouts
        if (audioCodecCtx)
        {
            av_channel_layout_uninit(&audioCodecCtx->ch_layout);
        }
        avformat_free_context(formatCtx);
        m_ffmpeg.formatCtx = nullptr;
        m_ffmpeg.formatBuffer = nullptr; // Liberado pelo avio_context_free
        m_ffmpeg.formatBufferSize = 0;
    }

    m_ffmpeg.videoStream = nullptr;
    m_ffmpeg.audioStream = nullptr;
}

void HTTPTSStreamer::flushCodecs()
{
    AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_ffmpeg.formatCtx);
    AVCodecContext *videoCodecCtx = static_cast<AVCodecContext *>(m_ffmpeg.videoCodecCtx);
    AVCodecContext *audioCodecCtx = static_cast<AVCodecContext *>(m_ffmpeg.audioCodecCtx);

    if (!formatCtx || !videoCodecCtx || !audioCodecCtx)
    {
        return;
    }

    // Flush video codec
    avcodec_send_frame(videoCodecCtx, nullptr);
    AVPacket *pkt = av_packet_alloc();
    while (avcodec_receive_packet(videoCodecCtx, pkt) >= 0)
    {
        pkt->stream_index = 0; // Video stream
        muxPacket(pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    // Flush audio codec
    avcodec_send_frame(audioCodecCtx, nullptr);
    pkt = av_packet_alloc();
    while (avcodec_receive_packet(audioCodecCtx, pkt) >= 0)
    {
        pkt->stream_index = 1; // Audio stream
        muxPacket(pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    // Write trailer
    av_write_trailer(formatCtx);
    avio_flush(formatCtx->pb);
}

bool HTTPTSStreamer::encodeVideoFrame(const uint8_t *rgbData, uint32_t width, uint32_t height)
{
    AVCodecContext *videoCodecCtx = static_cast<AVCodecContext *>(m_ffmpeg.videoCodecCtx);
    AVFrame *videoFrame = static_cast<AVFrame *>(m_ffmpeg.videoFrame);
    SwsContext *swsCtx = static_cast<SwsContext *>(m_ffmpeg.swsCtx);
    AVStream *videoStream = static_cast<AVStream *>(m_ffmpeg.videoStream);

    if (!videoCodecCtx || !videoFrame || !swsCtx || !videoStream)
    {
        return false;
    }

    // Make frame writable
    if (av_frame_make_writable(videoFrame) < 0)
    {
        return false;
    }

    // Convert RGB to YUV
    const uint8_t *srcData[1] = {rgbData};
    int srcLinesize[1] = {static_cast<int>(width * 3)};

    sws_scale(swsCtx, srcData, srcLinesize, 0, static_cast<int>(height),
              videoFrame->data, videoFrame->linesize);

    // Set PTS usando wall clock para sincronização precisa
    AVRational timeBase = videoStream->time_base;

    // PTS simples baseado em contador incremental
    // time_base é {1, fps}, então PTS = frameCount
    m_ffmpeg.videoFrameCount++;
    int64_t ptsInTimeBase = m_ffmpeg.videoFrameCount;

    // Garantir que PTS sempre avance
    if (ptsInTimeBase <= m_ffmpeg.videoPts)
    {
        ptsInTimeBase = m_ffmpeg.videoPts + 1;
        m_ffmpeg.videoFrameCount = ptsInTimeBase;
    }

    videoFrame->pts = ptsInTimeBase;
    m_ffmpeg.videoPts = ptsInTimeBase;

    // Forçar keyframe se necessário (primeiro frame ou periodicamente)
    if (m_ffmpeg.forceKeyFrame)
    {
        videoFrame->pict_type = AV_PICTURE_TYPE_I;
        // key_frame é deprecated, usar apenas pict_type
        m_ffmpeg.forceKeyFrame = false;
    }
    else
    {
        // Forçar keyframe periodicamente baseado no FPS configurado
        // Intervalo de keyframes = m_fps / 2 (0.5 segundos)
        int keyframeInterval = std::max(1, static_cast<int>(m_fps / 2));
        if (m_ffmpeg.videoFrameCount % keyframeInterval == 0)
        {
            videoFrame->pict_type = AV_PICTURE_TYPE_I;
            // key_frame é deprecated, usar apenas pict_type
        }
    }

    // Encode frame
    int ret = avcodec_send_frame(videoCodecCtx, videoFrame);
    if (ret < 0)
    {
        return false;
    }

    AVPacket *pkt = av_packet_alloc();
    while (avcodec_receive_packet(videoCodecCtx, pkt) >= 0)
    {
        pkt->stream_index = 0; // Video stream

        // Garantir DTS monotônico e PTS >= DTS
        if (m_ffmpeg.lastVideoDts >= 0)
        {
            int64_t minDtsIncrement = av_rescale_q(1, {1, static_cast<int>(m_fps)}, timeBase);
            if (pkt->dts <= m_ffmpeg.lastVideoDts)
            {
                pkt->dts = m_ffmpeg.lastVideoDts + minDtsIncrement;
            }
            // Garantir que PTS >= DTS
            if (pkt->pts < pkt->dts)
            {
                pkt->pts = pkt->dts;
            }
        }
        m_ffmpeg.lastVideoDts = pkt->dts;

        // IMPORTANTE: Não falhar se muxPacket retornar false (pode ser EAGAIN)
        // Continuar processando mesmo se o muxing não conseguir escrever imediatamente
        muxPacket(pkt); // Sempre chamar, mas não falhar se retornar false
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    return true;
}

bool HTTPTSStreamer::encodeAudioFrame(const int16_t *samples, size_t sampleCount)
{
    AVCodecContext *audioCodecCtx = static_cast<AVCodecContext *>(m_ffmpeg.audioCodecCtx);
    AVFrame *audioFrame = static_cast<AVFrame *>(m_ffmpeg.audioFrame);
    SwrContext *swrCtx = static_cast<SwrContext *>(m_ffmpeg.swrCtx);
    AVStream *audioStream = static_cast<AVStream *>(m_ffmpeg.audioStream);

    if (!audioCodecCtx || !audioFrame || !swrCtx || !audioStream)
    {
        return false;
    }

    // Make frame writable
    if (av_frame_make_writable(audioFrame) < 0)
    {
        return false;
    }

    // Convert int16 PCM to float planar
    const uint8_t *srcData[1] = {reinterpret_cast<const uint8_t *>(samples)};

    int ret = swr_convert(swrCtx, audioFrame->data, audioFrame->nb_samples,
                          srcData, static_cast<int>(sampleCount / m_channels));
    if (ret < 0)
    {
        return false;
    }

    audioFrame->nb_samples = ret;

    // PTS simples baseado em contador de samples
    // time_base é {1, sampleRate}, então PTS = sampleCount
    AVRational timeBase = audioStream->time_base;
    int64_t samplesInFrame = static_cast<int64_t>(sampleCount / m_channels);
    m_ffmpeg.audioSampleCount += samplesInFrame;
    int64_t ptsInTimeBase = av_rescale_q(m_ffmpeg.audioSampleCount, {1, static_cast<int>(m_sampleRate)}, timeBase);

    // Garantir que PTS sempre avance
    if (ptsInTimeBase <= m_ffmpeg.audioPts)
    {
        ptsInTimeBase = m_ffmpeg.audioPts + 1;
    }

    audioFrame->pts = ptsInTimeBase;
    m_ffmpeg.audioPts = ptsInTimeBase;

    // Encode frame
    ret = avcodec_send_frame(audioCodecCtx, audioFrame);
    if (ret < 0)
    {
        return false;
    }

    AVPacket *pkt = av_packet_alloc();
    while (avcodec_receive_packet(audioCodecCtx, pkt) >= 0)
    {
        pkt->stream_index = 1; // Audio stream

        // Garantir DTS monotônico e PTS >= DTS
        if (m_ffmpeg.lastAudioDts >= 0)
        {
            int64_t minDtsIncrement = av_rescale_q(1, {1, static_cast<int>(m_sampleRate)}, timeBase);
            if (pkt->dts <= m_ffmpeg.lastAudioDts)
            {
                pkt->dts = m_ffmpeg.lastAudioDts + minDtsIncrement;
            }
            // Garantir que PTS >= DTS
            if (pkt->pts < pkt->dts)
            {
                pkt->pts = pkt->dts;
            }
        }
        m_ffmpeg.lastAudioDts = pkt->dts;

        // IMPORTANTE: Não falhar se muxPacket retornar false (pode ser EAGAIN)
        // Continuar processando mesmo se o muxing não conseguir escrever imediatamente
        muxPacket(pkt); // Sempre chamar, mas não falhar se retornar false
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    return true;
}

bool HTTPTSStreamer::muxPacket(void *packet)
{
    AVPacket *pkt = static_cast<AVPacket *>(packet);
    AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_ffmpeg.formatCtx);

    if (!pkt || !formatCtx)
    {
        return false;
    }

    // CRÍTICO: av_interleaved_write_frame NÃO é thread-safe!
    // Precisamos proteger com mutex para evitar corrupção de pacotes
    std::lock_guard<std::mutex> muxLock(m_muxMutex);

    // IMPORTANTE: Usar av_interleaved_write_frame para garantir ordem correta dos pacotes
    // Isso é crítico para MPEG-TS e evita erros de decodificação
    int ret = av_interleaved_write_frame(formatCtx, pkt);
    if (ret < 0)
    {
        // EAGAIN significa buffer cheio - não é erro, apenas buffer cheio
        // Retornar sucesso mesmo assim para não bloquear o encoding
        if (ret == AVERROR(EAGAIN))
        {
            return true; // Buffer cheio, mas não é erro - continuar processando
        }

        // Outros erros são críticos e devem ser logados
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        LOG_ERROR("Erro ao fazer mux de pacote: " + std::string(errbuf) + " (código: " + std::to_string(ret) + ")");
        // Retornar true mesmo assim para não bloquear completamente, mas logar o erro
    }
    return true; // Sempre retornar sucesso para não bloquear o encoding
}

void HTTPTSStreamer::encodingThread()
{
    // Thread única que processa vídeo e áudio de forma intercalada
    // Abordagem simples: processar 1 frame de vídeo, depois processar áudio suficiente para esse frame
    AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_ffmpeg.formatCtx);
    AVCodecContext *videoCodecCtx = static_cast<AVCodecContext *>(m_ffmpeg.videoCodecCtx);
    AVCodecContext *audioCodecCtx = static_cast<AVCodecContext *>(m_ffmpeg.audioCodecCtx);

    if (!formatCtx || !videoCodecCtx || !audioCodecCtx)
    {
        LOG_ERROR("FFmpeg context não inicializado no encoding thread");
        return;
    }

    // Calcular intervalo entre frames de vídeo (microsegundos)
    int64_t videoFrameIntervalUs = 1000000LL / static_cast<int64_t>(m_fps);

    // Calcular samples de áudio por frame de vídeo
    size_t samplesPerVideoFrame = static_cast<size_t>(m_sampleRate / m_fps) * m_channels;
    size_t samplesPerAudioFrame = m_ffmpeg.audioFrameSize * m_channels;

    struct timespec lastVideoTime;
    clock_gettime(CLOCK_MONOTONIC, &lastVideoTime);

    while (m_running && !m_cleanedUp.load())
    {
        if (m_cleanedUp.load())
        {
            break;
        }

        struct timespec currentTime;
        clock_gettime(CLOCK_MONOTONIC, &currentTime);

        // Calcular tempo desde o último frame de vídeo (microsegundos)
        int64_t elapsedUs = ((currentTime.tv_sec - lastVideoTime.tv_sec) * 1000000LL) +
                            ((currentTime.tv_nsec - lastVideoTime.tv_nsec) / 1000LL);

        // Processar frame de vídeo se passou tempo suficiente
        if (elapsedUs >= videoFrameIntervalUs)
        {
            // Processar TODOS os frames disponíveis até alcançar a taxa correta
            // Se estamos atrasados, processar múltiplos frames para recuperar
            int framesToProcess = 1;
            if (elapsedUs > videoFrameIntervalUs * 2)
            {
                // Se estamos muito atrasados, processar múltiplos frames
                framesToProcess = static_cast<int>(elapsedUs / videoFrameIntervalUs);
                framesToProcess = std::min(framesToProcess, 5); // Limitar a 5 frames por vez
            }

            int framesProcessed = 0;
            for (int i = 0; i < framesToProcess; i++)
            {
                VideoFrame videoFrame;
                bool hasVideo = false;

                {
                    std::lock_guard<std::mutex> videoLock(m_videoMutex);
                    if (!m_videoQueue.empty())
                    {
                        videoFrame = std::move(m_videoQueue.front());
                        m_videoQueue.pop();
                        hasVideo = true;
                    }
                }

                if (hasVideo)
                {
                    encodeVideoFrame(videoFrame.data.data(), videoFrame.width, videoFrame.height);
                    framesProcessed++;
                }
                else
                {
                    break; // Não há mais frames
                }
            }

            if (framesProcessed > 0)
            {
                clock_gettime(CLOCK_MONOTONIC, &lastVideoTime);
            }
            else
            {
                // Sem frame de vídeo, mas atualizar timer
                lastVideoTime = currentTime;
            }

            // 2. Processar áudio correspondente a este frame de vídeo
            // Processar todos os frames de áudio disponíveis até cobrir o tempo do frame de vídeo
            size_t audioSamplesNeeded = samplesPerVideoFrame;
            size_t audioSamplesProcessed = 0;

            while (audioSamplesProcessed < audioSamplesNeeded)
            {
                bool hasEnoughAudio = false;

                {
                    std::lock_guard<std::mutex> audioLock(m_audioMutex);
                    hasEnoughAudio = (m_audioBuffer.size() >= samplesPerAudioFrame);
                }

                if (hasEnoughAudio)
                {
                    std::vector<int16_t> audioData(samplesPerAudioFrame);

                    {
                        std::lock_guard<std::mutex> audioLock(m_audioMutex);
                        if (m_audioBuffer.size() >= samplesPerAudioFrame)
                        {
                            std::copy(m_audioBuffer.begin(),
                                      m_audioBuffer.begin() + samplesPerAudioFrame,
                                      audioData.begin());
                            m_audioBuffer.erase(m_audioBuffer.begin(),
                                                m_audioBuffer.begin() + samplesPerAudioFrame);
                        }
                        else
                        {
                            break; // Alguém pegou antes
                        }
                    }

                    encodeAudioFrame(audioData.data(), samplesPerAudioFrame);
                    audioSamplesProcessed += samplesPerAudioFrame;
                }
                else
                {
                    // Não há áudio suficiente, mas continuar
                    break;
                }
            }
        }
        else
        {
            // Ainda não é hora - calcular quanto tempo falta
            int64_t waitUs = videoFrameIntervalUs - elapsedUs;
            if (waitUs > 0)
            {
                usleep(static_cast<useconds_t>(waitUs));
            }
        }
    }
}
