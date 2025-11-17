#include "HTTPTSStreamer.h"
#include "../utils/Logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <errno.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

// Helper macros para simplificar verificações de erro do FFmpeg
#define CHECK_FFMPEG_RET(ret, msg)                         \
    do                                                     \
    {                                                      \
        if ((ret) < 0)                                     \
        {                                                  \
            char errbuf[256];                              \
            av_strerror((ret), errbuf, sizeof(errbuf));    \
            LOG_ERROR((msg) + std::string(": ") + errbuf); \
            cleanupFFmpeg();                               \
            return false;                                  \
        }                                                  \
    } while (0)

#define CHECK_NULL(ptr, msg) \
    do                       \
    {                        \
        if (!(ptr))          \
        {                    \
            LOG_ERROR(msg);  \
            cleanupFFmpeg(); \
            return false;    \
        }                    \
    } while (0)

#define CHECK_FFMPEG_RET_NO_CLEANUP(ret, msg)              \
    do                                                     \
    {                                                      \
        if ((ret) < 0)                                     \
        {                                                  \
            char errbuf[256];                              \
            av_strerror((ret), errbuf, sizeof(errbuf));    \
            LOG_ERROR((msg) + std::string(": ") + errbuf); \
            return false;                                  \
        }                                                  \
    } while (0)

// Static callback for AVIO write
static int avioWriteCallback(void *opaque, const uint8_t *buf, int buf_size)
{
    HTTPTSStreamer *self = static_cast<HTTPTSStreamer *>(opaque);
    if (!self || !buf || buf_size <= 0)
    {
        return -1;
    }
    // Write to output buffer and send to all clients
    // Retornar o número de bytes escritos para indicar sucesso
    int written = self->writeToClients(buf, buf_size);
    return written > 0 ? written : -1;
}

HTTPTSStreamer::HTTPTSStreamer()
{
    // Initialize FFmpeg structures
    m_ffmpeg = FFmpegContext{};
    // Inicializar PTS de áudio e vídeo para evitar problemas de sincronização
    m_ffmpeg.videoPts = 0;
    m_ffmpeg.audioPts = 0;
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

    LOG_INFO("HTTP MPEG-TS Streamer inicializado: " + std::to_string(width) + "x" + std::to_string(height) +
             " @ " + std::to_string(fps) + "fps, porta " + std::to_string(port) +
             ", áudio: " + std::to_string(m_sampleRate) + "Hz, " + std::to_string(m_channels) + " canais");

    return initializeFFmpeg();
}

void HTTPTSStreamer::setAudioFormat(uint32_t sampleRate, uint32_t channels)
{
    m_sampleRate = sampleRate;
    m_channels = channels;
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

bool HTTPTSStreamer::initializeFFmpeg()
{
    // Initialize format context for MPEG-TS
    // Usar buffer mínimo (1 pacote TS) para forçar envio imediato
    m_ffmpeg.formatBufferSize = 188; // 1 pacote TS (188 bytes) - mínimo possível
    m_ffmpeg.formatBuffer = (uint8_t *)av_malloc(m_ffmpeg.formatBufferSize);
    if (!m_ffmpeg.formatBuffer)
    {
        LOG_ERROR("Falha ao alocar buffer para format context");
        return false;
    }

    // Criar AVIO context com write callback
    // O buffer será usado pelo FFmpeg para escrever dados
    AVIOContext *ioCtx = avio_alloc_context(
        m_ffmpeg.formatBuffer, m_ffmpeg.formatBufferSize, 1, this,
        nullptr,           // read function (not needed for output)
        avioWriteCallback, // write function
        nullptr);          // seek function (not needed)

    // Configurar para não usar buffer interno do AVIO (write imediato)
    ioCtx->write_flag = 1;
    ioCtx->direct = 1;            // Modo direto - escreve imediatamente sem buffering
    ioCtx->min_packet_size = 188; // Tamanho mínimo de pacote TS

    if (!ioCtx)
    {
        LOG_ERROR("Falha ao criar AVIO context");
        return false;
    }

    // Allocate format context
    AVFormatContext *formatCtx = nullptr;
    int ret = avformat_alloc_output_context2(&formatCtx, nullptr, "mpegts", nullptr);
    m_ffmpeg.formatCtx = formatCtx;
    if (ret < 0 || !formatCtx)
    {
        LOG_ERROR("Falha ao criar format context para MPEG-TS");
        avio_context_free(&ioCtx);
        return false;
    }
    // CHECK_FFMPEG_RET(ret, "Falha ao criar format context para MPEG-TS");
    // CHECK_NULL(formatCtx, "Falha ao criar format context para MPEG-TS");

    formatCtx->pb = ioCtx;
    // Note: We can't modify oformat->flags directly, but AVFMT_NOFILE is set automatically for custom IO

    // Initialize video codec (configurável)
    AVCodecID videoCodecID = AV_CODEC_ID_H264; // Padrão
    if (m_videoCodecName == "h264" || m_videoCodecName == "libx264")
    {
        videoCodecID = AV_CODEC_ID_H264;
    }
    else if (m_videoCodecName == "h265" || m_videoCodecName == "hevc" || m_videoCodecName == "libx265")
    {
        videoCodecID = AV_CODEC_ID_H265;
    }
    else if (m_videoCodecName == "vp8" || m_videoCodecName == "libvpx")
    {
        videoCodecID = AV_CODEC_ID_VP8;
    }
    else if (m_videoCodecName == "vp9" || m_videoCodecName == "libvpx-vp9")
    {
        videoCodecID = AV_CODEC_ID_VP9;
    }
    else
    {
        LOG_WARN("Codec de vídeo '" + m_videoCodecName + "' não reconhecido, usando H.264");
        videoCodecID = AV_CODEC_ID_H264;
    }

    const AVCodec *videoCodec = avcodec_find_encoder(videoCodecID);
    CHECK_NULL(videoCodec, "Codec de vídeo não encontrado: " + m_videoCodecName);

    AVStream *videoStream = avformat_new_stream(formatCtx, videoCodec);
    m_ffmpeg.videoStream = videoStream;
    CHECK_NULL(videoStream, "Falha ao criar video stream");

    AVCodecContext *videoCodecCtx = avcodec_alloc_context3(videoCodec);
    m_ffmpeg.videoCodecCtx = videoCodecCtx;
    CHECK_NULL(videoCodecCtx, "Falha ao alocar video codec context");

    videoCodecCtx->codec_id = videoCodecID;
    videoCodecCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    videoCodecCtx->width = m_width;
    videoCodecCtx->height = m_height;
    videoCodecCtx->time_base = {1, static_cast<int>(m_fps)};
    videoCodecCtx->framerate = {static_cast<int>(m_fps), 1};
    videoCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    videoCodecCtx->bit_rate = m_videoBitrate;
    videoCodecCtx->gop_size = 1;     // GOP mínimo (1 = apenas I-frames ou I+P) para menor latência
    videoCodecCtx->max_b_frames = 0; // Desabilitar B-frames para menor latência

    // Set codec options para baixa latência
    av_opt_set(videoCodecCtx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(videoCodecCtx->priv_data, "tune", "zerolatency", 0);
    av_opt_set(videoCodecCtx->priv_data, "profile", "baseline", 0);   // Baseline profile para menor latência
    av_opt_set_int(videoCodecCtx->priv_data, "rc-lookahead", 0, 0);   // Sem lookahead
    av_opt_set_int(videoCodecCtx->priv_data, "sliced-threads", 1, 0); // Threading para menor latência

    ret = avcodec_open2(videoCodecCtx, videoCodec, nullptr);
    CHECK_FFMPEG_RET(ret, "Falha ao abrir codec H.264");

    ret = avcodec_parameters_from_context(videoStream->codecpar, videoCodecCtx);
    CHECK_FFMPEG_RET(ret, "Falha ao copiar parâmetros do codec");

    videoStream->time_base = videoCodecCtx->time_base;

    // Initialize audio codec (configurável)
    AVCodecID audioCodecID = AV_CODEC_ID_AAC; // Padrão
    if (m_audioCodecName == "aac" || m_audioCodecName == "libfdk_aac")
    {
        audioCodecID = AV_CODEC_ID_AAC;
    }
    else if (m_audioCodecName == "mp3" || m_audioCodecName == "libmp3lame")
    {
        audioCodecID = AV_CODEC_ID_MP3;
    }
    else if (m_audioCodecName == "opus" || m_audioCodecName == "libopus")
    {
        audioCodecID = AV_CODEC_ID_OPUS;
    }
    else
    {
        LOG_WARN("Codec de áudio '" + m_audioCodecName + "' não reconhecido, usando AAC");
        audioCodecID = AV_CODEC_ID_AAC;
    }

    const AVCodec *audioCodec = avcodec_find_encoder(audioCodecID);
    CHECK_NULL(audioCodec, "Codec de áudio não encontrado: " + m_audioCodecName);

    AVStream *audioStream = avformat_new_stream(formatCtx, audioCodec);
    m_ffmpeg.audioStream = audioStream;
    CHECK_NULL(audioStream, "Falha ao criar audio stream");

    AVCodecContext *audioCodecCtx = avcodec_alloc_context3(audioCodec);
    m_ffmpeg.audioCodecCtx = audioCodecCtx;
    CHECK_NULL(audioCodecCtx, "Falha ao alocar audio codec context");

    audioCodecCtx->codec_id = audioCodecID;
    audioCodecCtx->codec_type = AVMEDIA_TYPE_AUDIO;
    audioCodecCtx->sample_rate = m_sampleRate;
    // Use new channel layout API (FFmpeg 5.0+)
    av_channel_layout_default(&audioCodecCtx->ch_layout, m_channels);
    audioCodecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    audioCodecCtx->bit_rate = m_audioBitrate;
    // Time base baseado na sample rate para sincronização correta
    audioCodecCtx->time_base = {1, static_cast<int>(m_sampleRate)};
    // Configurar para baixa latência
    audioCodecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    ret = avcodec_open2(audioCodecCtx, audioCodec, nullptr);
    CHECK_FFMPEG_RET(ret, "Falha ao abrir codec AAC");

    ret = avcodec_parameters_from_context(audioStream->codecpar, audioCodecCtx);
    CHECK_FFMPEG_RET(ret, "Falha ao copiar parâmetros do audio codec");

    audioStream->time_base = audioCodecCtx->time_base;
    m_ffmpeg.audioFrameSize = audioCodecCtx->frame_size;

    // Allocate frames
    AVFrame *videoFrame = av_frame_alloc();
    AVFrame *audioFrame = av_frame_alloc();
    m_ffmpeg.videoFrame = videoFrame;
    m_ffmpeg.audioFrame = audioFrame;

    if (!videoFrame || !audioFrame)
    {
        LOG_ERROR("Falha ao alocar frames");
        cleanupFFmpeg();
        return false;
    }
    // CHECK_NULL não funciona bem com múltiplos ponteiros, mantendo if explícito

    videoFrame->format = videoCodecCtx->pix_fmt;
    videoFrame->width = videoCodecCtx->width;
    videoFrame->height = videoCodecCtx->height;

    ret = av_frame_get_buffer(videoFrame, 32);
    CHECK_FFMPEG_RET(ret, "Falha ao alocar buffer do video frame");

    audioFrame->format = audioCodecCtx->sample_fmt;
    audioFrame->sample_rate = audioCodecCtx->sample_rate;
    av_channel_layout_copy(&audioFrame->ch_layout, &audioCodecCtx->ch_layout);
    audioFrame->nb_samples = m_ffmpeg.audioFrameSize;

    ret = av_frame_get_buffer(audioFrame, 0);
    CHECK_FFMPEG_RET(ret, "Falha ao alocar buffer do audio frame");

    // Initialize SWS context for RGB to YUV conversion
    SwsContext *swsCtx = sws_getContext(
        m_width, m_height, AV_PIX_FMT_RGB24,
        m_width, m_height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    m_ffmpeg.swsCtx = swsCtx;

    CHECK_NULL(swsCtx, "Falha ao criar SWS context");

    // Initialize SWR context for audio resampling
    AVChannelLayout inChLayout, outChLayout;
    av_channel_layout_default(&inChLayout, m_channels);
    av_channel_layout_default(&outChLayout, m_channels);
    SwrContext *swrCtx = nullptr;
    swr_alloc_set_opts2(&swrCtx,
                        &outChLayout, audioCodecCtx->sample_fmt, audioCodecCtx->sample_rate,
                        &inChLayout, AV_SAMPLE_FMT_S16, m_sampleRate,
                        0, nullptr);
    m_ffmpeg.swrCtx = swrCtx;

    if (!swrCtx)
    {
        LOG_ERROR("Falha ao criar SWR context");
        av_channel_layout_uninit(&inChLayout);
        av_channel_layout_uninit(&outChLayout);
        cleanupFFmpeg();
        return false;
    }

    ret = swr_init(swrCtx);
    if (ret < 0)
    {
        LOG_ERROR("Falha ao inicializar SWR context");
        av_channel_layout_uninit(&inChLayout);
        av_channel_layout_uninit(&outChLayout);
        cleanupFFmpeg();
        return false;
    }

    av_channel_layout_uninit(&inChLayout);
    av_channel_layout_uninit(&outChLayout);

    // Configurar opções de formato para baixa latência
    AVDictionary *opts = nullptr;
    av_dict_set_int(&opts, "muxrate", 0, 0);       // Sem limite de bitrate no muxer
    av_dict_set_int(&opts, "flush_packets", 1, 0); // Flush imediato
    av_dict_set_int(&opts, "pcr_period", 5, 0);    // PCR muito frequente (5ms) para menor latência
    av_dict_set_int(&opts, "max_delay", 0, 0);     // Sem delay máximo - enviar imediatamente

    av_dict_set(&formatCtx->metadata, "service_name", "RetroCapture", 0);
    av_dict_set(&formatCtx->metadata, "service_provider", "RetroCapture", 0);

    // Write header com opções de baixa latência
    ret = avformat_write_header(formatCtx, &opts);
    if (ret < 0)
    {
        LOG_ERROR("Falha ao escrever header MPEG-TS");
        av_dict_free(&opts);
        cleanupFFmpeg();
        return false;
    }
    av_dict_free(&opts);

    LOG_INFO("FFmpeg inicializado para MPEG-TS streaming");
    return true;
}

void HTTPTSStreamer::cleanupFFmpeg()
{
    if (m_ffmpeg.swsCtx)
    {
        sws_freeContext(static_cast<SwsContext *>(m_ffmpeg.swsCtx));
        m_ffmpeg.swsCtx = nullptr;
    }

    if (m_ffmpeg.swrCtx)
    {
        SwrContext *swrCtx = static_cast<SwrContext *>(m_ffmpeg.swrCtx);
        swr_free(&swrCtx);
        m_ffmpeg.swrCtx = nullptr;
    }

    if (m_ffmpeg.videoFrame)
    {
        AVFrame *frame = static_cast<AVFrame *>(m_ffmpeg.videoFrame);
        av_frame_free(&frame);
        m_ffmpeg.videoFrame = nullptr;
    }

    if (m_ffmpeg.audioFrame)
    {
        AVFrame *frame = static_cast<AVFrame *>(m_ffmpeg.audioFrame);
        av_frame_free(&frame);
        m_ffmpeg.audioFrame = nullptr;
    }

    if (m_ffmpeg.videoCodecCtx)
    {
        AVCodecContext *ctx = static_cast<AVCodecContext *>(m_ffmpeg.videoCodecCtx);
        avcodec_free_context(&ctx);
        m_ffmpeg.videoCodecCtx = nullptr;
    }

    if (m_ffmpeg.audioCodecCtx)
    {
        AVCodecContext *ctx = static_cast<AVCodecContext *>(m_ffmpeg.audioCodecCtx);
        avcodec_free_context(&ctx);
        m_ffmpeg.audioCodecCtx = nullptr;
    }

    if (m_ffmpeg.formatCtx)
    {
        AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_ffmpeg.formatCtx);
        if (formatCtx->pb)
        {
            avio_context_free(&formatCtx->pb);
        }
        avformat_free_context(formatCtx);
        m_ffmpeg.formatCtx = nullptr;
    }

    if (m_ffmpeg.formatBuffer)
    {
        av_free(m_ffmpeg.formatBuffer);
        m_ffmpeg.formatBuffer = nullptr;
    }
}

bool HTTPTSStreamer::start()
{
    if (m_active)
    {
        return true;
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

    LOG_INFO("HTTP MPEG-TS Streamer parado");
}

bool HTTPTSStreamer::isActive() const
{
    return m_active;
}

bool HTTPTSStreamer::pushFrame(const uint8_t *data, uint32_t width, uint32_t height)
{
    if (!data || !m_active)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_frameMutex);

    // IMPORTANTE: Para reduzir buffering, descartar frames antigos e manter apenas o mais recente
    // Isso garante que sempre processamos o frame mais atual, reduzindo latência
    // Limitar a fila a no máximo 1 frame para evitar acúmulo e buffering
    while (m_videoFrames.size() >= 1)
    {
        m_videoFrames.pop(); // Descartar frame antigo
    }

    // Store frame
    size_t frameSize = width * height * 3;
    std::vector<uint8_t> frame(frameSize);
    memcpy(frame.data(), data, frameSize);
    m_videoFrames.push(std::move(frame));
    m_frameWidth = width;
    m_frameHeight = height;

    return true;
}

bool HTTPTSStreamer::pushAudio(const int16_t *samples, size_t sampleCount)
{
    if (!samples || !m_active || sampleCount == 0)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_audioMutex);

    // Store audio samples
    std::vector<int16_t> audioSamples(sampleCount);
    memcpy(audioSamples.data(), samples, sampleCount * sizeof(int16_t));
    m_audioSamples.push(std::move(audioSamples));

    return true;
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
    cleanupFFmpeg();
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
    m_clientCount++;
    LOG_INFO("Cliente HTTP MPEG-TS conectado (total: " + std::to_string(m_clientCount.load()) + ")");

    // Configurar socket para baixa latência
    int flag = 1;
    setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)); // Desabilitar Nagle's algorithm

    // Reduzir buffer de envio para forçar envio imediato
    int sendBufSize = 8192; // 8KB buffer pequeno
    setsockopt(clientFd, SOL_SOCKET, SO_SNDBUF, &sendBufSize, sizeof(sendBufSize));

    // Add client to list
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        m_clientSockets.push_back(clientFd);
    }

    // Read HTTP request
    char buffer[4096];
    ssize_t bytesRead = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
    if (bytesRead <= 0)
    {
        close(clientFd);
        m_clientCount--;
        {
            std::lock_guard<std::mutex> lock(m_outputMutex);
            m_clientSockets.erase(
                std::remove(m_clientSockets.begin(), m_clientSockets.end(), clientFd),
                m_clientSockets.end());
        }
        return;
    }

    buffer[bytesRead] = '\0';

    // Check if it's a GET request
    std::string request(buffer);
    if (request.find("GET /stream") == std::string::npos &&
        request.find("GET /") == std::string::npos)
    {
        const char *response = "HTTP/1.1 404 Not Found\r\n\r\n";
        send(clientFd, response, strlen(response), 0);
        close(clientFd);
        m_clientCount--;
        {
            std::lock_guard<std::mutex> lock(m_outputMutex);
            m_clientSockets.erase(
                std::remove(m_clientSockets.begin(), m_clientSockets.end(), clientFd),
                m_clientSockets.end());
        }
        return;
    }

    // Send HTTP headers for MPEG-TS stream
    std::ostringstream headers;
    headers << "HTTP/1.1 200 OK\r\n";
    headers << "Content-Type: video/mp2t\r\n";
    headers << "Connection: keep-alive\r\n";
    headers << "Cache-Control: no-cache\r\n";
    headers << "\r\n";

    std::string headerStr = headers.str();
    ssize_t sent = send(clientFd, headerStr.c_str(), headerStr.length(), MSG_NOSIGNAL);
    if (sent < 0)
    {
        close(clientFd);
        m_clientCount--;
        {
            std::lock_guard<std::mutex> lock(m_outputMutex);
            m_clientSockets.erase(
                std::remove(m_clientSockets.begin(), m_clientSockets.end(), clientFd),
                m_clientSockets.end());
        }
        return;
    }

    // Stream MPEG-TS data
    // Data is sent via avioWriteCallback when packets are muxed
    // Just keep the connection alive and check if client is still connected
    while (m_running)
    {
        // Check if client is still connected
        char test;
        if (recv(clientFd, &test, 1, MSG_PEEK | MSG_DONTWAIT) <= 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                break; // Client disconnected
            }
        }

        // Sleep briefly to avoid busy-waiting
        usleep(10000); // 10ms
    }

    close(clientFd);
    m_clientCount--;
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        m_clientSockets.erase(
            std::remove(m_clientSockets.begin(), m_clientSockets.end(), clientFd),
            m_clientSockets.end());
    }
    LOG_INFO("Cliente HTTP MPEG-TS desconectado (total: " + std::to_string(m_clientCount.load()) + ")");
}

void HTTPTSStreamer::encodingThread()
{
    // This will encode video and audio frames and mux them into MPEG-TS
    AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_ffmpeg.formatCtx);
    AVCodecContext *videoCodecCtx = static_cast<AVCodecContext *>(m_ffmpeg.videoCodecCtx);
    AVCodecContext *audioCodecCtx = static_cast<AVCodecContext *>(m_ffmpeg.audioCodecCtx);

    if (!formatCtx || !videoCodecCtx || !audioCodecCtx)
    {
        LOG_ERROR("FFmpeg context não inicializado no encoding thread");
        return;
    }

    while (m_running)
    {
        bool hasVideo = false;
        bool hasAudio = false;

        // Check for video frame
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            hasVideo = !m_videoFrames.empty();
        }

        // Check for audio samples (verificar tanto a fila quanto o buffer de acumulação)
        {
            std::lock_guard<std::mutex> lock(m_audioMutex);
            hasAudio = !m_audioSamples.empty() || !m_audioBuffer.empty();
        }

        // IMPORTANTE: Processar vídeo primeiro para reduzir latência
        // Vídeo tem prioridade porque frames antigos são descartados
        // Processar vídeo primeiro garante que sempre temos o frame mais recente
        if (hasVideo)
        {
            std::vector<uint8_t> frameData;
            uint32_t width, height;

            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                if (!m_videoFrames.empty())
                {
                    frameData = std::move(m_videoFrames.front());
                    m_videoFrames.pop();
                    width = m_frameWidth;
                    height = m_frameHeight;
                }
            }

            if (!frameData.empty())
            {
                encodeVideoFrame(frameData.data(), width, height);
            }
        }

        // Processar áudio após vídeo para manter sincronização
        if (hasAudio)
        {
            // Processar todos os samples disponíveis para evitar atraso
            while (true)
            {
                std::vector<int16_t> audioData;

                {
                    std::lock_guard<std::mutex> lock(m_audioMutex);
                    if (!m_audioSamples.empty())
                    {
                        audioData = std::move(m_audioSamples.front());
                        m_audioSamples.pop();
                    }
                    else
                    {
                        break; // Não há mais samples na fila
                    }
                }

                if (!audioData.empty())
                {
                    encodeAudioFrame(audioData.data(), audioData.size());
                }
            }

            // Também tentar processar frames completos do buffer de acumulação
            // Mesmo sem novos samples, pode haver frames completos esperando
            {
                std::lock_guard<std::mutex> lock(m_audioMutex);
                size_t samplesPerFrame = m_ffmpeg.audioFrameSize * m_channels;
                if (m_audioBuffer.size() >= samplesPerFrame)
                {
                    // Há frames completos - processar chamando encodeAudioFrame com dados vazios
                    // Isso vai processar o buffer de acumulação
                    encodeAudioFrame(nullptr, 0);
                }
            }
        }

        // Fazer flush periódico para garantir que dados pendentes sejam enviados
        AVFormatContext *fmtCtx = static_cast<AVFormatContext *>(m_ffmpeg.formatCtx);
        if (fmtCtx && fmtCtx->pb)
        {
            avio_flush(fmtCtx->pb);
        }

        // If no data available, sleep briefly
        if (!hasVideo && !hasAudio)
        {
            usleep(1000); // 1ms
        }
    }
}

bool HTTPTSStreamer::encodeVideoFrame(const uint8_t *rgbData, uint32_t width, uint32_t height)
{
    if (!rgbData || !m_active)
    {
        return false;
    }

    AVCodecContext *videoCodecCtx = static_cast<AVCodecContext *>(m_ffmpeg.videoCodecCtx);
    AVFrame *videoFrame = static_cast<AVFrame *>(m_ffmpeg.videoFrame);
    SwsContext *swsCtx = static_cast<SwsContext *>(m_ffmpeg.swsCtx);
    AVStream *videoStream = static_cast<AVStream *>(m_ffmpeg.videoStream);

    if (!videoCodecCtx || !videoFrame || !swsCtx || !videoStream)
    {
        return false;
    }

    // Convert RGB to YUV420P
    const uint8_t *srcData[1] = {rgbData};
    int srcLinesize[1] = {static_cast<int>(width * 3)};

    int ret = sws_scale(swsCtx, srcData, srcLinesize, 0, height,
                        videoFrame->data, videoFrame->linesize);
    if (ret < 0)
    {
        LOG_ERROR("Falha ao converter RGB para YUV");
        return false;
    }

    // Set frame properties
    // Usar PTS baseado no frame number para garantir continuidade
    videoFrame->pts = m_ffmpeg.videoPts;
    // Incrementar PTS baseado no time_base do codec
    int64_t ptsIncrement = av_rescale_q(1, {1, static_cast<int>(m_fps)}, videoCodecCtx->time_base);
    m_ffmpeg.videoPts += ptsIncrement;

    // Garantir que o PTS não seja negativo
    if (videoFrame->pts < 0)
    {
        videoFrame->pts = 0;
        m_ffmpeg.videoPts = ptsIncrement;
    }

    // Encode frame
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
    {
        return false;
    }

    ret = avcodec_send_frame(videoCodecCtx, videoFrame);
    if (ret < 0)
    {
        av_packet_free(&pkt);
        return false;
    }

    while (ret >= 0)
    {
        ret = avcodec_receive_packet(videoCodecCtx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            break;
        }
        else if (ret < 0)
        {
            av_packet_free(&pkt);
            return false;
        }

        // Mux packet
        pkt->stream_index = videoStream->index;
        // Rescalar PTS para o time_base do stream
        av_packet_rescale_ts(pkt, videoCodecCtx->time_base, videoStream->time_base);
        // Garantir que DTS <= PTS para evitar problemas de buffering
        if (pkt->dts > pkt->pts)
        {
            pkt->dts = pkt->pts;
        }
        muxPacket(pkt);

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return true;
}

bool HTTPTSStreamer::encodeAudioFrame(const int16_t *samples, size_t sampleCount)
{
    if (!m_active)
    {
        return false;
    }

    AVCodecContext *audioCodecCtx = static_cast<AVCodecContext *>(m_ffmpeg.audioCodecCtx);
    AVFrame *audioFrame = static_cast<AVFrame *>(m_ffmpeg.audioFrame);
    SwrContext *swrCtx = static_cast<SwrContext *>(m_ffmpeg.swrCtx);
    AVStream *audioStream = static_cast<AVStream *>(m_ffmpeg.audioStream);

    if (!audioCodecCtx || !audioFrame || !swrCtx || !audioStream)
    {
        return false;
    }

    // Adicionar samples ao buffer de acumulação (se fornecidos)
    if (sampleCount > 0 && samples)
    {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        m_audioBuffer.insert(m_audioBuffer.end(), samples, samples + sampleCount);
    }

    // Acumular samples no buffer até ter um frame completo
    // AAC geralmente precisa de 1024 samples por canal (2048 samples total para stereo)
    size_t samplesPerFrame = m_ffmpeg.audioFrameSize * m_channels;

    // Processar frames completos
    bool encoded = false;
    while (true)
    {
        size_t availableSamples = 0;
        {
            std::lock_guard<std::mutex> lock(m_audioMutex);
            availableSamples = m_audioBuffer.size();
        }

        if (availableSamples < samplesPerFrame)
        {
            break; // Não há samples suficientes para um frame completo
        }

        // Extrair um frame completo
        std::vector<int16_t> frameSamples(samplesPerFrame);
        {
            std::lock_guard<std::mutex> lock(m_audioMutex);
            std::copy(m_audioBuffer.begin(), m_audioBuffer.begin() + samplesPerFrame, frameSamples.begin());
            m_audioBuffer.erase(m_audioBuffer.begin(), m_audioBuffer.begin() + samplesPerFrame);
        }

        // Resample and convert audio
        // IMPORTANTE: swr_convert espera samples de entrada no formato de entrada (S16)
        // e converte para o formato de saída (FLTP para AAC)
        const uint8_t *srcData[1] = {reinterpret_cast<const uint8_t *>(frameSamples.data())};
        const int srcSampleCount = static_cast<int>(m_ffmpeg.audioFrameSize); // Samples de entrada

        // Garantir que o frame está configurado corretamente
        audioFrame->nb_samples = m_ffmpeg.audioFrameSize;

        int ret = swr_convert(swrCtx, audioFrame->data, audioFrame->nb_samples,
                              srcData, srcSampleCount);
        if (ret < 0)
        {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Falha ao converter áudio: " + std::string(errbuf));
            break;
        }

        if (ret == 0)
        {
            // Nenhum sample convertido, pode ser um problema
            break;
        }

        // Ajustar nb_samples se necessário (geralmente ret == audioFrameSize)
        if (ret != m_ffmpeg.audioFrameSize)
        {
            audioFrame->nb_samples = ret;
        }

        // Set frame properties
        // PTS baseado no número de samples de entrada processados
        // O PTS deve refletir o tempo real baseado nos samples de entrada
        audioFrame->pts = m_ffmpeg.audioPts;
        // Incrementar PTS baseado no número de samples de ENTRADA processados
        // m_ffmpeg.audioFrameSize é o número de samples por frame (entrada e saída são iguais neste caso)
        // O PTS é incrementado pelo número de samples processados
        int64_t samplesConsumed = m_ffmpeg.audioFrameSize; // Samples de entrada consumidos (por canal)
        // Para stereo, temos samplesConsumed * 2 samples totais, mas o PTS é por sample
        m_ffmpeg.audioPts += samplesConsumed;

        // Garantir que o PTS não seja negativo
        if (audioFrame->pts < 0)
        {
            audioFrame->pts = 0;
            m_ffmpeg.audioPts = samplesConsumed;
        }

        // Encode frame
        AVPacket *pkt = av_packet_alloc();
        if (!pkt)
        {
            break;
        }

        ret = avcodec_send_frame(audioCodecCtx, audioFrame);
        if (ret < 0)
        {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Falha ao enviar frame de áudio para encoder: " + std::string(errbuf));
            av_packet_free(&pkt);
            break;
        }

        while (ret >= 0)
        {
            ret = avcodec_receive_packet(audioCodecCtx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                break;
            }
            else if (ret < 0)
            {
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("Falha ao receber pacote de áudio do encoder: " + std::string(errbuf));
                av_packet_free(&pkt);
                return false;
            }

            // Mux packet
            pkt->stream_index = audioStream->index;
            // Rescalar PTS para o time_base do stream
            av_packet_rescale_ts(pkt, audioCodecCtx->time_base, audioStream->time_base);
            // Garantir que DTS <= PTS para evitar problemas de buffering
            if (pkt->dts > pkt->pts)
            {
                pkt->dts = pkt->pts;
            }
            // Garantir que DTS não seja negativo
            if (pkt->dts < 0)
            {
                pkt->dts = pkt->pts;
            }
            muxPacket(pkt);

            av_packet_unref(pkt);
        }

        av_packet_free(&pkt);
        encoded = true;
    }

    return encoded;
}

bool HTTPTSStreamer::muxPacket(void *packet)
{
    if (!packet || !m_active)
    {
        return false;
    }

    AVPacket *pkt = static_cast<AVPacket *>(packet);
    AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_ffmpeg.formatCtx);

    if (!formatCtx || !pkt)
    {
        return false;
    }

    // Usar av_interleaved_write_frame para garantir intercalação correta de áudio/vídeo
    // Isso é essencial para MPEG-TS - sem intercalação, o player pode descartar pacotes
    int ret = av_interleaved_write_frame(formatCtx, pkt);
    if (ret < 0)
    {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Falha ao escrever pacote MPEG-TS: " + std::string(errbuf));
        return false;
    }

    // Flush imediato e agressivo do AVIO context para forçar envio aos clientes
    // Isso é crítico para reduzir latência mesmo com intercalação
    AVIOContext *pb = formatCtx->pb;
    if (pb)
    {
        // Flush múltiplas vezes para garantir que todos os dados sejam enviados
        avio_flush(pb);
        // Também forçar flush do buffer interno se existir
        if (pb->buf_end > pb->buf_ptr)
        {
            // Há dados no buffer - forçar escrita
            avio_flush(pb);
        }
    }

    return true;
}

int HTTPTSStreamer::writeToClients(const uint8_t *buf, int buf_size)
{
    if (!buf || buf_size <= 0)
    {
        return -1;
    }

    std::lock_guard<std::mutex> lock(m_outputMutex);

    // Send to all connected clients
    // Enviar em chunks pequenos para evitar bloqueios e reduzir latência
    auto it = m_clientSockets.begin();
    while (it != m_clientSockets.end())
    {
        int clientFd = *it;

        // Enviar todos os dados, garantindo que tudo seja enviado
        const uint8_t *data = buf;
        size_t remaining = buf_size;

        while (remaining > 0)
        {
            ssize_t sent = send(clientFd, data, remaining, MSG_NOSIGNAL);

            if (sent < 0)
            {
                // Erro ao enviar - cliente desconectado
                close(clientFd);
                it = m_clientSockets.erase(it);
                m_clientCount--;
                goto next_client;
            }

            data += sent;
            remaining -= sent;
        }

        ++it;
    next_client:;
    }

    return buf_size;
}
