#include "HTTPTSStreamer.h"
#include "../utils/Logger.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
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

// Função auxiliar para obter timestamp em microssegundos (para depuração de sincronização)
static int64_t getTimestampUs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000LL + static_cast<int64_t>(ts.tv_nsec) / 1000LL;
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
    TimestampedFrame frame;
    frame.data = std::make_shared<std::vector<uint8_t>>(data, data + width * height * 3);
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
        m_timestampedVideoBuffer.push_back(frame);
        m_latestVideoTimestampUs = std::max(m_latestVideoTimestampUs, captureTimestampUs);

        // Limpar dados antigos se necessário (baseado em tempo)
        cleanupOldData();
    }

    return true;
}

bool HTTPTSStreamer::pushAudio(const int16_t *samples, size_t sampleCount)
{
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

    // Inicializar FFmpeg primeiro
    if (!initializeFFmpeg())
    {
        LOG_ERROR("Failed to initialize FFmpeg");
        return false;
    }

    // Criar socket do servidor HTTP
    m_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSocket < 0)
    {
        LOG_ERROR("Failed to create server socket: " + std::string(strerror(errno)));
        cleanupFFmpeg();
        return false;
    }

    // Configurar opções do socket
    int opt = 1;
    setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Configurar endereço
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_port);

    // Fazer bind
    if (bind(m_serverSocket, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        LOG_ERROR("Failed to bind to port " + std::to_string(m_port) + ": " + std::string(strerror(errno)));
        close(m_serverSocket);
        m_serverSocket = -1;
        cleanupFFmpeg();
        return false;
    }

    // Fazer listen
    if (listen(m_serverSocket, 5) < 0)
    {
        LOG_ERROR("Failed to listen on socket: " + std::string(strerror(errno)));
        close(m_serverSocket);
        m_serverSocket = -1;
        cleanupFFmpeg();
        return false;
    }

    // Iniciar threads
    m_running = true;
    m_active = true;

    m_serverThread = std::thread(&HTTPTSStreamer::serverThread, this);
    m_encodingThread = std::thread(&HTTPTSStreamer::encodingThread, this);

    LOG_INFO("HTTP TS Streamer started on port " + std::to_string(m_port));
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

    // Fechar socket do servidor para acordar accept()
    if (m_serverSocket >= 0)
    {
        close(m_serverSocket);
        m_serverSocket = -1;
    }

    // Fechar todos os sockets de clientes
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        for (int clientFd : m_clientSockets)
        {
            close(clientFd);
        }
        m_clientSockets.clear();
        m_clientCount = 0;
    }

    // Aguardar threads terminarem
    if (m_serverThread.joinable())
    {
        m_serverThread.join();
    }
    if (m_encodingThread.joinable())
    {
        m_encodingThread.join();
    }

    // Limpar FFmpeg
    flushCodecs();
    cleanupFFmpeg();

    LOG_INFO("HTTP TS Streamer stopped");
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

void HTTPTSStreamer::handleClient(int clientFd)
{
    // Configurar socket para baixa latência
    int flag = 1;
    setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Ler requisição HTTP
    char buffer[4096];
    ssize_t bytesRead = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
    if (bytesRead <= 0)
    {
        close(clientFd);
        return;
    }

    buffer[bytesRead] = '\0';
    std::string request(buffer);

    // Verificar se é uma requisição GET /stream
    bool isStreamRequest = (request.find("GET /stream") != std::string::npos);

    if (!isStreamRequest)
    {
        // Enviar 404 para requisições que não são /stream
        const char *response = "HTTP/1.1 404 Not Found\r\n\r\n";
        send(clientFd, response, strlen(response), 0);
        close(clientFd);
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
    ssize_t sent = send(clientFd, headerStr.c_str(), headerStr.length(), MSG_NOSIGNAL);
    if (sent < 0)
    {
        // Cliente desconectou ou erro
        close(clientFd);
        return;
    }

    // Enviar header do formato MPEG-TS se já foi capturado
    {
        std::lock_guard<std::mutex> headerLock(m_headerMutex);
        if (m_headerWritten && !m_formatHeader.empty())
        {
            LOG_INFO("Sending MPEG-TS format header to new client (" +
                     std::to_string(m_formatHeader.size()) + " bytes)");
            ssize_t headerSent = send(clientFd, m_formatHeader.data(), m_formatHeader.size(), MSG_NOSIGNAL);
            if (headerSent < 0)
            {
                LOG_ERROR("Failed to send format header to client");
                close(clientFd);
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
    while (m_running)
    {
        char dummy;
        ssize_t result = recv(clientFd, &dummy, 1, MSG_PEEK);
        if (result <= 0)
        {
            break; // Cliente desconectou
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // evitar busy-waiting
    }

    // Cliente desconectou - remover da lista
    close(clientFd);
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

int HTTPTSStreamer::writeToClients(const uint8_t *buf, int buf_size)
{
    if (!buf || buf_size <= 0)
    {
        return buf_size; // Dados inválidos, mas retornar sucesso
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
            }
        }
    }

    // Enviar dados diretamente para todos os clientes (sem buffer, sem sincronização)
    std::lock_guard<std::mutex> lock(m_outputMutex);

    if (m_clientSockets.empty())
    {
        // Sem clientes conectados, mas retornar sucesso para não bloquear o FFmpeg
        return buf_size;
    }

    // Enviar dados para todos os clientes conectados
    // Remover clientes que falharam ao enviar
    auto it = m_clientSockets.begin();
    while (it != m_clientSockets.end())
    {
        int clientFd = *it;

        // Enviar dados em chunks se necessário
        size_t totalSent = 0;
        bool error = false;

        while (totalSent < static_cast<size_t>(buf_size) && !error)
        {
            size_t toSend = static_cast<size_t>(buf_size) - totalSent;
            // Limitar tamanho do chunk
            if (toSend > 65536)
            {
                toSend = 65536;
            }

            ssize_t sent = send(clientFd, buf + totalSent, toSend, MSG_NOSIGNAL);
            if (sent < 0)
            {
                int err = errno;
                // EPIPE e ECONNRESET são normais quando cliente desconecta
                // EAGAIN/EWOULDBLOCK significa que o socket não está pronto
                if (err != EAGAIN && err != EWOULDBLOCK)
                {
                    error = true;
                    close(clientFd);
                    it = m_clientSockets.erase(it);
                    m_clientCount = m_clientSockets.size();
                    break;
                }
                else
                {
                    // Socket não está pronto, tentar novamente
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            }
            else if (sent == 0)
            {
                // Cliente fechou a conexão
                error = true;
                close(clientFd);
                it = m_clientSockets.erase(it);
                m_clientCount = m_clientSockets.size();
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
    codecCtx->gop_size = 1;
    codecCtx->max_b_frames = 0;
    codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    codecCtx->bit_rate = m_videoBitrate;
    codecCtx->thread_count = 4;

    // Abrir codec
    if (avcodec_open2(codecCtx, codec, nullptr) < 0)
    {
        LOG_ERROR("Failed to open video codec");
        avcodec_free_context(&codecCtx);
        return false;
    }

    m_videoCodecContext = codecCtx;

    // Criar SWS context para conversão RGB -> YUV
    SwsContext *swsCtx = sws_getContext(
        m_width, m_height, AV_PIX_FMT_RGB24,
        m_width, m_height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!swsCtx)
    {
        LOG_ERROR("Failed to create SWS context");
        avcodec_free_context(&codecCtx);
        m_videoCodecContext = nullptr;
        return false;
    }
    m_swsContext = swsCtx;

    // Alocar frame de vídeo
    AVFrame *videoFrame = av_frame_alloc();
    if (!videoFrame)
    {
        LOG_ERROR("Failed to allocate video frame");
        sws_freeContext(swsCtx);
        avcodec_free_context(&codecCtx);
        m_videoCodecContext = nullptr;
        m_swsContext = nullptr;
        return false;
    }

    videoFrame->format = AV_PIX_FMT_YUV420P;
    videoFrame->width = m_width;
    videoFrame->height = m_height;
    if (av_frame_get_buffer(videoFrame, 0) < 0)
    {
        LOG_ERROR("Failed to allocate video frame buffer");
        av_frame_free(&videoFrame);
        sws_freeContext(swsCtx);
        avcodec_free_context(&codecCtx);
        m_videoCodecContext = nullptr;
        m_swsContext = nullptr;
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
    m_audioStream = audioStream;

    // Configurar callback de escrita
    formatCtx->pb = avio_alloc_context(
        static_cast<unsigned char *>(av_malloc(64 * 1024)), 64 * 1024,
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
        avcodec_free_context(&ctx);
        m_videoCodecContext = nullptr;
    }
    if (audioCtx)
    {
        AVCodecContext *ctx = audioCtx;
        avcodec_free_context(&ctx);
        m_audioCodecContext = nullptr;
    }
    if (formatCtx)
    {
        // Escrever trailer antes de fechar
        av_write_trailer(formatCtx);

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
        m_muxerContext = nullptr;
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

    SwsContext *swsCtx = static_cast<SwsContext *>(m_swsContext);
    AVFrame *frame = static_cast<AVFrame *>(videoFrame);

    if (!swsCtx || !frame)
    {
        return false;
    }

    if (av_frame_make_writable(frame) < 0)
    {
        return false;
    }

    const uint8_t *srcData[1] = {rgbData};
    int srcLinesize[1] = {static_cast<int>(width * 3)};

    sws_scale(swsCtx, srcData, srcLinesize, 0, height,
              frame->data, frame->linesize);

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
    videoFrame->pts = static_cast<int64_t>(relativeTimeSeconds * timeBase.den / timeBase.num);

    // Forçar primeiro frame como keyframe
    static int64_t frameCount = 0;
    if (frameCount == 0)
    {
        videoFrame->key_frame = 1;
        videoFrame->pict_type = AV_PICTURE_TYPE_I;
        LOG_INFO("encodeVideoFrame: First frame set as keyframe");
    }
    frameCount++;

    // Enviar frame para codec
    int ret = avcodec_send_frame(codecCtx, videoFrame);
    if (ret < 0)
    {
        LOG_ERROR("encodeVideoFrame: avcodec_send_frame failed: " + std::to_string(ret));
        return false;
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

    // Tentar receber pacotes múltiplas vezes para garantir que todos sejam processados
    // Isso é especialmente importante para o primeiro keyframe
    int maxAttempts = (frameCount == 1 && videoFrame->key_frame == 1) ? 10 : 1;

    for (int attempt = 0; attempt < maxAttempts; attempt++)
    {
        int ret = avcodec_receive_packet(codecCtx, pkt);
        if (ret < 0)
        {
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                // Codec precisa de mais frames ou terminou
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
        // Configurar stream_index do pacote
        pkt->stream_index = videoStream->index;

        // Converter PTS/DTS do time_base do codec para o time_base do stream
        if (pkt->pts != AV_NOPTS_VALUE)
        {
            pkt->pts = av_rescale_q(pkt->pts, codecCtx->time_base, videoStream->time_base);
        }
        if (pkt->dts != AV_NOPTS_VALUE)
        {
            pkt->dts = av_rescale_q(pkt->dts, codecCtx->time_base, videoStream->time_base);
        }

        // Log detalhado para depuração de sincronização (primeiros 10 frames e depois a cada segundo)
        static int videoEncodeLogCount = 0;
        static int64_t firstVideoEncodeTime = 0;
        int64_t muxTimeUs = getTimestampUs();
        if (videoEncodeLogCount == 0)
        {
            firstVideoEncodeTime = muxTimeUs;
        }
        if (videoEncodeLogCount < 10 || (videoEncodeLogCount % 60 == 0))
        {
            int64_t elapsedUs = muxTimeUs - firstVideoEncodeTime;
            LOG_INFO("[SYNC_DEBUG] [VIDEO_ENCODE] Frame #" + std::to_string(frameCount - 1) +
                     " encoded at " + std::to_string(muxTimeUs) + "us, elapsed=" +
                     std::to_string(elapsedUs) + "us (" + std::to_string(elapsedUs / 1000) + "ms)" +
                     ", capture_ts=" + std::to_string(captureTimestampUs) + "us" +
                     ", PTS=" + std::to_string(pkt->pts) + ", DTS=" + std::to_string(pkt->dts));
        }
        videoEncodeLogCount++;

        // Muxar e enviar pacote diretamente (já foi removido da fila bruta antes de chamar esta função)
        if (!muxPacket(pkt))
        {
            LOG_ERROR("encodeVideoFrame: muxPacket failed");
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    if (packetCount == 0)
    {
        if (frameCount == 1)
        {
            LOG_WARN("encodeVideoFrame: First frame produced no packets (codec may need more frames)");
        }
        else
        {
            // Log removido para reduzir poluição
        }
    }
    else if (frameCount == 1 && packetCount > 0)
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

    bool encodedAny = false;
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
        audioFrame->pts = static_cast<int64_t>(relativeTimeSeconds * timeBase.den / timeBase.num);

        int64_t frameEncodeTimeUs = getTimestampUs();
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

        encodedAny = true;

        // Receber pacotes encodados e muxar/enviar diretamente
        AVPacket *pkt = av_packet_alloc();
        int packetCount = 0;
        while (avcodec_receive_packet(codecCtx, pkt) >= 0)
        {
            packetCount++;
            // Configurar stream_index do pacote
            pkt->stream_index = audioStream->index;

            // Converter PTS/DTS do time_base do codec para o time_base do stream
            if (pkt->pts != AV_NOPTS_VALUE)
            {
                pkt->pts = av_rescale_q(pkt->pts, codecCtx->time_base, audioStream->time_base);
            }
            if (pkt->dts != AV_NOPTS_VALUE)
            {
                pkt->dts = av_rescale_q(pkt->dts, codecCtx->time_base, audioStream->time_base);
            }

            // Log detalhado para depuração de sincronização (primeiros 10 frames e depois a cada segundo)
            // Atualizar último PTS de áudio processado
            if (pkt->pts != AV_NOPTS_VALUE)
            {
            }

            static int audioEncodeLogCount = 0;
            static int64_t firstAudioEncodeTime = 0;
            int64_t muxTimeUs = getTimestampUs();
            if (audioEncodeLogCount == 0)
            {
                firstAudioEncodeTime = muxTimeUs;
            }
            if (audioEncodeLogCount < 10 || (audioEncodeLogCount % 43 == 0)) // ~1 segundo a 44100Hz/1024
            {
                int64_t elapsedUs = muxTimeUs - firstAudioEncodeTime;
                double audioDurationMs = (static_cast<double>(samplesPerFrame) / static_cast<double>(m_audioSampleRate)) * 1000.0;
                std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
                size_t accumulatorSize = m_audioAccumulator.size();
                LOG_INFO("[SYNC_DEBUG] [AUDIO_ENCODE] Frame #" + std::to_string(frameCount - 1) +
                         " encoded at " + std::to_string(muxTimeUs) + "us, elapsed=" +
                         std::to_string(elapsedUs) + "us (" + std::to_string(elapsedUs / 1000) + "ms)" +
                         ", capture_ts=" + std::to_string(captureTimestampUs) + "us" +
                         ", PTS=" + std::to_string(pkt->pts) + ", DTS=" + std::to_string(pkt->dts) +
                         ", duration=" + std::to_string(audioDurationMs) + "ms" +
                         ", accumulator_size=" + std::to_string(accumulatorSize));
            }
            audioEncodeLogCount++;

            // Muxar e enviar pacote diretamente
            if (!muxPacket(pkt))
            {
                LOG_ERROR("encodeAudioFrame: muxPacket failed");
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);

        if (packetCount > 0)
        {
            encodedAny = true;
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
    if (!packet)
    {
        return false;
    }
    AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_muxerContext);
    if (!formatCtx)
    {
        return false;
    }
    AVPacket *pkt = static_cast<AVPacket *>(packet);

    // Garantir que DTS seja válido e monotônico
    if (pkt->dts == AV_NOPTS_VALUE)
    {
        // Se DTS não está definido, usar PTS
        if (pkt->pts != AV_NOPTS_VALUE)
        {
            pkt->dts = pkt->pts;
        }
        else
        {
            LOG_ERROR("muxPacket: Both PTS and DTS are invalid");
            return false;
        }
    }

    // Garantir que DTS <= PTS (requisito do MPEG-TS)
    if (pkt->pts != AV_NOPTS_VALUE && pkt->dts > pkt->pts)
    {
        pkt->dts = pkt->pts;
    }

    int ret = av_interleaved_write_frame(formatCtx, pkt);
    if (ret < 0)
    {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to write packet (stream=" + std::to_string(pkt->stream_index) +
                  ", pts=" + std::to_string(pkt->pts) + ", dts=" + std::to_string(pkt->dts) +
                  "): " + std::string(errbuf));
        return false;
    }

    return true;
}

void HTTPTSStreamer::serverThread()
{
    LOG_INFO("Server thread started, waiting for connections on port " + std::to_string(m_port));

    while (m_running)
    {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);

        // Aceitar conexão de cliente
        int clientFd = accept(m_serverSocket, (struct sockaddr *)&clientAddr, &clientLen);
        if (clientFd < 0)
        {
            if (m_running)
            {
                // Erro ao aceitar (pode ser porque fechamos o socket)
                if (errno != EBADF && errno != EINVAL)
                {
                    LOG_ERROR("Failed to accept client connection: " + std::string(strerror(errno)));
                }
            }
            break; // Sair do loop se não estiver rodando ou se o socket foi fechado
        }

        // Processar cliente em thread separada
        std::thread clientThread(&HTTPTSStreamer::handleClient, this, clientFd);
        clientThread.detach(); // Detach para não precisar fazer join
    }

    LOG_INFO("Server thread stopped");
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

    // Calcular timestamp mais antigo permitido (janela temporal)
    int64_t oldestAllowedVideoUs = m_latestVideoTimestampUs - MAX_BUFFER_TIME_US;
    int64_t oldestAllowedAudioUs = m_latestAudioTimestampUs - MAX_BUFFER_TIME_US;

    // Remover frames de vídeo mais antigos que a janela temporal
    while (!m_timestampedVideoBuffer.empty())
    {
        const auto &frame = m_timestampedVideoBuffer.front();

        // Se está dentro da janela temporal, manter
        if (frame.captureTimestampUs >= oldestAllowedVideoUs)
        {
            break;
        }

        // Se está fora da janela, descartar (mesmo que não processado - dados muito antigos)
        m_timestampedVideoBuffer.pop_front();
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

    // Verificar se há sobreposição suficiente (mínimo 1 segundo)
    if (overlapEndUs - overlapStartUs < MIN_BUFFER_TIME_US)
    {
        return SyncZone::invalid(); // Aguardar mais dados
    }

    // Zona sincronizada: meio da sobreposição (não as pontas)
    // Pegar do meio para garantir que há dados suficientes antes e depois
    int64_t zoneStartUs = overlapStartUs + (overlapEndUs - overlapStartUs) / 4;   // 25% do início
    int64_t zoneEndUs = overlapStartUs + 3 * (overlapEndUs - overlapStartUs) / 4; // 75% do início

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

void HTTPTSStreamer::encodingThread()
{
    while (m_running)
    {
        // Calcular zona de sincronização
        SyncZone zone = calculateSyncZone();

        if (!zone.isValid())
        {
            // Não há zona válida ainda - AGUARDAR (não descartar)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Processar dados da zona sincronizada
        {
            std::lock_guard<std::mutex> videoLock(m_videoBufferMutex);

            // Processar frames de vídeo da zona sincronizada
            for (size_t i = zone.videoStartIdx; i < zone.videoEndIdx && i < m_timestampedVideoBuffer.size(); i++)
            {
                auto &frame = m_timestampedVideoBuffer[i];
                if (!frame.processed && frame.data && frame.width > 0 && frame.height > 0)
                {
                    encodeVideoFrame(frame.data->data(), frame.width, frame.height, frame.captureTimestampUs);
                    frame.processed = true; // Marcar como processado
                }
            }
        }

        {
            std::lock_guard<std::mutex> audioLock(m_audioBufferMutex);

            // Processar chunks de áudio da zona sincronizada
            for (size_t i = zone.audioStartIdx; i < zone.audioEndIdx && i < m_timestampedAudioBuffer.size(); i++)
            {
                auto &audio = m_timestampedAudioBuffer[i];
                if (!audio.processed && audio.samples && audio.sampleCount > 0)
                {
                    encodeAudioFrame(audio.samples->data(), audio.sampleCount, audio.captureTimestampUs);
                    audio.processed = true; // Marcar como processado
                }
            }
        }

        // Limpar dados processados (remover do buffer após envio bem-sucedido)
        {
            std::lock_guard<std::mutex> videoLock(m_videoBufferMutex);
            while (!m_timestampedVideoBuffer.empty() && m_timestampedVideoBuffer.front().processed)
            {
                m_timestampedVideoBuffer.pop_front();
            }
        }

        {
            std::lock_guard<std::mutex> audioLock(m_audioBufferMutex);
            while (!m_timestampedAudioBuffer.empty() && m_timestampedAudioBuffer.front().processed)
            {
                m_timestampedAudioBuffer.pop_front();
            }
        }

        // Aguardar um pouco antes de recalcular a zona
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
