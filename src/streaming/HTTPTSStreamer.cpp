#include "HTTPTSStreamer.h"
#include "../utils/Logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
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

// Static callback for AVIO write
static int avioWriteCallback(void* opaque, const uint8_t* buf, int buf_size) {
    HTTPTSStreamer* self = static_cast<HTTPTSStreamer*>(opaque);
    if (!self || !buf || buf_size <= 0) {
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
             " @ " + std::to_string(fps) + "fps, porta " + std::to_string(port));
    
    return initializeFFmpeg();
}

bool HTTPTSStreamer::initializeFFmpeg()
{
    // Initialize format context for MPEG-TS
    // Usar buffer pequeno para forçar envio frequente e reduzir latência
    m_ffmpeg.formatBufferSize = 188 * 7; // 7 pacotes TS (188 bytes cada) = ~1.3KB
    m_ffmpeg.formatBuffer = (uint8_t*)av_malloc(m_ffmpeg.formatBufferSize);
    if (!m_ffmpeg.formatBuffer) {
        LOG_ERROR("Falha ao alocar buffer para format context");
        return false;
    }
    
    // Criar AVIO context com write callback
    // O buffer será usado pelo FFmpeg para escrever dados
    AVIOContext* ioCtx = avio_alloc_context(
        m_ffmpeg.formatBuffer, m_ffmpeg.formatBufferSize, 1, this,
        nullptr, // read function (not needed for output)
        avioWriteCallback, // write function
        nullptr); // seek function (not needed)
    
    // Configurar para não usar buffer interno do AVIO (write imediato)
    ioCtx->write_flag = 1;
    ioCtx->direct = 1; // Modo direto - escreve imediatamente sem buffering
    
    if (!ioCtx) {
        LOG_ERROR("Falha ao criar AVIO context");
        return false;
    }
    
    // Allocate format context
    AVFormatContext* formatCtx = nullptr;
    int ret = avformat_alloc_output_context2(&formatCtx, nullptr, "mpegts", nullptr);
    m_ffmpeg.formatCtx = formatCtx;
    if (ret < 0 || !formatCtx) {
        LOG_ERROR("Falha ao criar format context para MPEG-TS");
        avio_context_free(&ioCtx);
        return false;
    }
    
    formatCtx->pb = ioCtx;
    // Note: We can't modify oformat->flags directly, but AVFMT_NOFILE is set automatically for custom IO
    
    // Initialize video codec (H.264)
    const AVCodec* videoCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!videoCodec) {
        LOG_ERROR("Codec H.264 não encontrado");
        cleanupFFmpeg();
        return false;
    }
    
    AVStream* videoStream = avformat_new_stream(formatCtx, videoCodec);
    m_ffmpeg.videoStream = videoStream;
    if (!videoStream) {
        LOG_ERROR("Falha ao criar video stream");
        cleanupFFmpeg();
        return false;
    }
    
    AVCodecContext* videoCodecCtx = avcodec_alloc_context3(videoCodec);
    m_ffmpeg.videoCodecCtx = videoCodecCtx;
    if (!videoCodecCtx) {
        LOG_ERROR("Falha ao alocar video codec context");
        cleanupFFmpeg();
        return false;
    }
    
    videoCodecCtx->codec_id = AV_CODEC_ID_H264;
    videoCodecCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    videoCodecCtx->width = m_width;
    videoCodecCtx->height = m_height;
    videoCodecCtx->time_base = {1, static_cast<int>(m_fps)};
    videoCodecCtx->framerate = {static_cast<int>(m_fps), 1};
    videoCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    videoCodecCtx->bit_rate = m_videoBitrate;
    videoCodecCtx->gop_size = 10;
    videoCodecCtx->max_b_frames = 0; // Desabilitar B-frames para menor latência
    
    // Set codec options para baixa latência
    av_opt_set(videoCodecCtx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(videoCodecCtx->priv_data, "tune", "zerolatency", 0);
    av_opt_set(videoCodecCtx->priv_data, "profile", "baseline", 0); // Baseline profile para menor latência
    av_opt_set_int(videoCodecCtx->priv_data, "rc-lookahead", 0, 0); // Sem lookahead
    av_opt_set_int(videoCodecCtx->priv_data, "sliced-threads", 1, 0); // Threading para menor latência
    
    ret = avcodec_open2(videoCodecCtx, videoCodec, nullptr);
    if (ret < 0) {
        LOG_ERROR("Falha ao abrir codec H.264");
        cleanupFFmpeg();
        return false;
    }
    
    ret = avcodec_parameters_from_context(videoStream->codecpar, videoCodecCtx);
    if (ret < 0) {
        LOG_ERROR("Falha ao copiar parâmetros do codec");
        cleanupFFmpeg();
        return false;
    }
    
    videoStream->time_base = videoCodecCtx->time_base;
    
    // Initialize audio codec (AAC)
    const AVCodec* audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!audioCodec) {
        LOG_ERROR("Codec AAC não encontrado");
        cleanupFFmpeg();
        return false;
    }
    
    AVStream* audioStream = avformat_new_stream(formatCtx, audioCodec);
    m_ffmpeg.audioStream = audioStream;
    if (!audioStream) {
        LOG_ERROR("Falha ao criar audio stream");
        cleanupFFmpeg();
        return false;
    }
    
    AVCodecContext* audioCodecCtx = avcodec_alloc_context3(audioCodec);
    m_ffmpeg.audioCodecCtx = audioCodecCtx;
    if (!audioCodecCtx) {
        LOG_ERROR("Falha ao alocar audio codec context");
        cleanupFFmpeg();
        return false;
    }
    
    audioCodecCtx->codec_id = AV_CODEC_ID_AAC;
    audioCodecCtx->codec_type = AVMEDIA_TYPE_AUDIO;
    audioCodecCtx->sample_rate = m_sampleRate;
    // Use new channel layout API (FFmpeg 5.0+)
    av_channel_layout_default(&audioCodecCtx->ch_layout, m_channels);
    audioCodecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    audioCodecCtx->bit_rate = m_audioBitrate;
    audioCodecCtx->time_base = {1, static_cast<int>(m_sampleRate)};
    
    ret = avcodec_open2(audioCodecCtx, audioCodec, nullptr);
    if (ret < 0) {
        LOG_ERROR("Falha ao abrir codec AAC");
        cleanupFFmpeg();
        return false;
    }
    
    ret = avcodec_parameters_from_context(audioStream->codecpar, audioCodecCtx);
    if (ret < 0) {
        LOG_ERROR("Falha ao copiar parâmetros do audio codec");
        cleanupFFmpeg();
        return false;
    }
    
    audioStream->time_base = audioCodecCtx->time_base;
    m_ffmpeg.audioFrameSize = audioCodecCtx->frame_size;
    
    // Allocate frames
    AVFrame* videoFrame = av_frame_alloc();
    AVFrame* audioFrame = av_frame_alloc();
    m_ffmpeg.videoFrame = videoFrame;
    m_ffmpeg.audioFrame = audioFrame;
    
    if (!videoFrame || !audioFrame) {
        LOG_ERROR("Falha ao alocar frames");
        cleanupFFmpeg();
        return false;
    }
    
    videoFrame->format = videoCodecCtx->pix_fmt;
    videoFrame->width = videoCodecCtx->width;
    videoFrame->height = videoCodecCtx->height;
    
    ret = av_frame_get_buffer(videoFrame, 32);
    if (ret < 0) {
        LOG_ERROR("Falha ao alocar buffer do video frame");
        cleanupFFmpeg();
        return false;
    }
    
    audioFrame->format = audioCodecCtx->sample_fmt;
    audioFrame->sample_rate = audioCodecCtx->sample_rate;
    av_channel_layout_copy(&audioFrame->ch_layout, &audioCodecCtx->ch_layout);
    audioFrame->nb_samples = m_ffmpeg.audioFrameSize;
    
    ret = av_frame_get_buffer(audioFrame, 0);
    if (ret < 0) {
        LOG_ERROR("Falha ao alocar buffer do audio frame");
        cleanupFFmpeg();
        return false;
    }
    
    // Initialize SWS context for RGB to YUV conversion
    SwsContext* swsCtx = sws_getContext(
        m_width, m_height, AV_PIX_FMT_RGB24,
        m_width, m_height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    m_ffmpeg.swsCtx = swsCtx;
    
    if (!swsCtx) {
        LOG_ERROR("Falha ao criar SWS context");
        cleanupFFmpeg();
        return false;
    }
    
    // Initialize SWR context for audio resampling
    AVChannelLayout inChLayout, outChLayout;
    av_channel_layout_default(&inChLayout, m_channels);
    av_channel_layout_default(&outChLayout, m_channels);
    SwrContext* swrCtx = nullptr;
    swr_alloc_set_opts2(&swrCtx,
        &outChLayout, audioCodecCtx->sample_fmt, audioCodecCtx->sample_rate,
        &inChLayout, AV_SAMPLE_FMT_S16, m_sampleRate,
        0, nullptr);
    m_ffmpeg.swrCtx = swrCtx;
    
    if (!swrCtx) {
        LOG_ERROR("Falha ao criar SWR context");
        av_channel_layout_uninit(&inChLayout);
        av_channel_layout_uninit(&outChLayout);
        cleanupFFmpeg();
        return false;
    }
    
    ret = swr_init(swrCtx);
    if (ret < 0) {
        LOG_ERROR("Falha ao inicializar SWR context");
        av_channel_layout_uninit(&inChLayout);
        av_channel_layout_uninit(&outChLayout);
        cleanupFFmpeg();
        return false;
    }
    
    av_channel_layout_uninit(&inChLayout);
    av_channel_layout_uninit(&outChLayout);
    
    // Configurar opções de formato para baixa latência
    AVDictionary* opts = nullptr;
    av_dict_set_int(&opts, "muxrate", 0, 0); // Sem limite de bitrate no muxer
    av_dict_set_int(&opts, "flush_packets", 1, 0); // Flush imediato
    av_dict_set_int(&opts, "pcr_period", 10, 0); // PCR muito frequente (10ms) para menor latência
    av_dict_set_int(&opts, "muxrate", 0, 0); // Sem limite de bitrate
    av_dict_set_int(&opts, "max_delay", 0, 0); // Sem delay máximo - enviar imediatamente
    
    av_dict_set(&formatCtx->metadata, "service_name", "RetroCapture", 0);
    av_dict_set(&formatCtx->metadata, "service_provider", "RetroCapture", 0);
    
    // Write header com opções de baixa latência
    ret = avformat_write_header(formatCtx, &opts);
    if (ret < 0) {
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
    if (m_ffmpeg.swsCtx) {
        sws_freeContext(static_cast<SwsContext*>(m_ffmpeg.swsCtx));
        m_ffmpeg.swsCtx = nullptr;
    }
    
    if (m_ffmpeg.swrCtx) {
        SwrContext* swrCtx = static_cast<SwrContext*>(m_ffmpeg.swrCtx);
        swr_free(&swrCtx);
        m_ffmpeg.swrCtx = nullptr;
    }
    
    if (m_ffmpeg.videoFrame) {
        AVFrame* frame = static_cast<AVFrame*>(m_ffmpeg.videoFrame);
        av_frame_free(&frame);
        m_ffmpeg.videoFrame = nullptr;
    }
    
    if (m_ffmpeg.audioFrame) {
        AVFrame* frame = static_cast<AVFrame*>(m_ffmpeg.audioFrame);
        av_frame_free(&frame);
        m_ffmpeg.audioFrame = nullptr;
    }
    
    if (m_ffmpeg.videoCodecCtx) {
        AVCodecContext* ctx = static_cast<AVCodecContext*>(m_ffmpeg.videoCodecCtx);
        avcodec_free_context(&ctx);
        m_ffmpeg.videoCodecCtx = nullptr;
    }
    
    if (m_ffmpeg.audioCodecCtx) {
        AVCodecContext* ctx = static_cast<AVCodecContext*>(m_ffmpeg.audioCodecCtx);
        avcodec_free_context(&ctx);
        m_ffmpeg.audioCodecCtx = nullptr;
    }
    
    if (m_ffmpeg.formatCtx) {
        AVFormatContext* formatCtx = static_cast<AVFormatContext*>(m_ffmpeg.formatCtx);
        if (formatCtx->pb) {
            avio_context_free(&formatCtx->pb);
        }
        avformat_free_context(formatCtx);
        m_ffmpeg.formatCtx = nullptr;
    }
    
    if (m_ffmpeg.formatBuffer) {
        av_free(m_ffmpeg.formatBuffer);
        m_ffmpeg.formatBuffer = nullptr;
    }
}

bool HTTPTSStreamer::start()
{
    if (m_active) {
        return true;
    }
    
    // Create server socket
    m_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSocket < 0) {
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
    
    if (bind(m_serverSocket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Falha ao fazer bind na porta " + std::to_string(m_port));
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }
    
    // Listen
    if (listen(m_serverSocket, 5) < 0) {
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
    if (!m_active) {
        return;
    }
    
    m_running = false;
    m_active = false;
    
    // Close server socket to wake up accept()
    if (m_serverSocket >= 0) {
        close(m_serverSocket);
        m_serverSocket = -1;
    }
    
    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }
    
    if (m_encodingThread.joinable()) {
        m_encodingThread.join();
    }
    
    LOG_INFO("HTTP MPEG-TS Streamer parado");
}

bool HTTPTSStreamer::isActive() const
{
    return m_active;
}

bool HTTPTSStreamer::pushFrame(const uint8_t* data, uint32_t width, uint32_t height)
{
    if (!data || !m_active) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(m_frameMutex);
    
    // Store frame
    size_t frameSize = width * height * 3;
    std::vector<uint8_t> frame(frameSize);
    memcpy(frame.data(), data, frameSize);
    m_videoFrames.push(std::move(frame));
    m_frameWidth = width;
    m_frameHeight = height;
    
    return true;
}

bool HTTPTSStreamer::pushAudio(const int16_t* samples, size_t sampleCount)
{
    if (!samples || !m_active || sampleCount == 0) {
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
        if (clientFd < 0) {
            if (m_running) {
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
    
    // Add client to list
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        m_clientSockets.push_back(clientFd);
    }
    
    // Read HTTP request
    char buffer[4096];
    ssize_t bytesRead = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
    if (bytesRead <= 0) {
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
        request.find("GET /") == std::string::npos) {
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
    if (sent < 0) {
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
    while (m_running) {
        // Check if client is still connected
        char test;
        if (recv(clientFd, &test, 1, MSG_PEEK | MSG_DONTWAIT) <= 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
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
    AVFormatContext* formatCtx = static_cast<AVFormatContext*>(m_ffmpeg.formatCtx);
    AVCodecContext* videoCodecCtx = static_cast<AVCodecContext*>(m_ffmpeg.videoCodecCtx);
    AVCodecContext* audioCodecCtx = static_cast<AVCodecContext*>(m_ffmpeg.audioCodecCtx);
    
    if (!formatCtx || !videoCodecCtx || !audioCodecCtx) {
        LOG_ERROR("FFmpeg context não inicializado no encoding thread");
        return;
    }
    
    while (m_running) {
        bool hasVideo = false;
        bool hasAudio = false;
        
        // Check for video frame
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            hasVideo = !m_videoFrames.empty();
        }
        
        // Check for audio samples
        {
            std::lock_guard<std::mutex> lock(m_audioMutex);
            hasAudio = !m_audioSamples.empty();
        }
        
        // Process video frame if available
        if (hasVideo) {
            std::vector<uint8_t> frameData;
            uint32_t width, height;
            
            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                if (!m_videoFrames.empty()) {
                    frameData = std::move(m_videoFrames.front());
                    m_videoFrames.pop();
                    width = m_frameWidth;
                    height = m_frameHeight;
                }
            }
            
            if (!frameData.empty()) {
                encodeVideoFrame(frameData.data(), width, height);
            }
        }
        
        // Process audio samples if available
        // Processar todos os samples disponíveis para evitar atraso
        while (true) {
            std::vector<int16_t> audioData;
            
            {
                std::lock_guard<std::mutex> lock(m_audioMutex);
                if (!m_audioSamples.empty()) {
                    audioData = std::move(m_audioSamples.front());
                    m_audioSamples.pop();
                } else {
                    break; // Não há mais samples
                }
            }
            
            if (!audioData.empty()) {
                encodeAudioFrame(audioData.data(), audioData.size());
            }
        }
        
        // If no data available, sleep briefly
        if (!hasVideo && !hasAudio) {
            usleep(1000); // 1ms
        }
    }
}

bool HTTPTSStreamer::encodeVideoFrame(const uint8_t* rgbData, uint32_t width, uint32_t height)
{
    if (!rgbData || !m_active) {
        return false;
    }
    
    AVCodecContext* videoCodecCtx = static_cast<AVCodecContext*>(m_ffmpeg.videoCodecCtx);
    AVFrame* videoFrame = static_cast<AVFrame*>(m_ffmpeg.videoFrame);
    SwsContext* swsCtx = static_cast<SwsContext*>(m_ffmpeg.swsCtx);
    AVStream* videoStream = static_cast<AVStream*>(m_ffmpeg.videoStream);
    
    if (!videoCodecCtx || !videoFrame || !swsCtx || !videoStream) {
        return false;
    }
    
    // Convert RGB to YUV420P
    const uint8_t* srcData[1] = {rgbData};
    int srcLinesize[1] = {static_cast<int>(width * 3)};
    
    int ret = sws_scale(swsCtx, srcData, srcLinesize, 0, height,
                        videoFrame->data, videoFrame->linesize);
    if (ret < 0) {
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
    if (videoFrame->pts < 0) {
        videoFrame->pts = 0;
        m_ffmpeg.videoPts = ptsIncrement;
    }
    
    // Encode frame
    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        return false;
    }
    
    ret = avcodec_send_frame(videoCodecCtx, videoFrame);
    if (ret < 0) {
        av_packet_free(&pkt);
        return false;
    }
    
    while (ret >= 0) {
        ret = avcodec_receive_packet(videoCodecCtx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            av_packet_free(&pkt);
            return false;
        }
        
        // Mux packet
        pkt->stream_index = videoStream->index;
        // Rescalar PTS para o time_base do stream
        av_packet_rescale_ts(pkt, videoCodecCtx->time_base, videoStream->time_base);
        // Garantir que DTS <= PTS para evitar problemas de buffering
        if (pkt->dts > pkt->pts) {
            pkt->dts = pkt->pts;
        }
        muxPacket(pkt);
        
        av_packet_unref(pkt);
    }
    
    av_packet_free(&pkt);
    return true;
}

bool HTTPTSStreamer::encodeAudioFrame(const int16_t* samples, size_t sampleCount)
{
    if (!samples || !m_active || sampleCount == 0) {
        return false;
    }
    
    AVCodecContext* audioCodecCtx = static_cast<AVCodecContext*>(m_ffmpeg.audioCodecCtx);
    AVFrame* audioFrame = static_cast<AVFrame*>(m_ffmpeg.audioFrame);
    SwrContext* swrCtx = static_cast<SwrContext*>(m_ffmpeg.swrCtx);
    AVStream* audioStream = static_cast<AVStream*>(m_ffmpeg.audioStream);
    
    if (!audioCodecCtx || !audioFrame || !swrCtx || !audioStream) {
        return false;
    }
    
    // Acumular samples no buffer até ter um frame completo
    // AAC geralmente precisa de 1024 samples por canal (2048 samples total para stereo)
    size_t samplesPerFrame = m_ffmpeg.audioFrameSize * m_channels;
    
    {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        m_audioBuffer.insert(m_audioBuffer.end(), samples, samples + sampleCount);
    }
    
    // Processar frames completos
    bool encoded = false;
    while (true) {
        size_t availableSamples = 0;
        {
            std::lock_guard<std::mutex> lock(m_audioMutex);
            availableSamples = m_audioBuffer.size();
        }
        
        if (availableSamples < samplesPerFrame) {
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
        const uint8_t* srcData[1] = {reinterpret_cast<const uint8_t*>(frameSamples.data())};
        const int srcSampleCount = static_cast<int>(m_ffmpeg.audioFrameSize);
        
        int ret = swr_convert(swrCtx, audioFrame->data, audioFrame->nb_samples,
                             srcData, srcSampleCount);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Falha ao converter áudio: " + std::string(errbuf));
            break;
        }
        
        if (ret == 0) {
            // Nenhum sample convertido, pode ser um problema
            break;
        }
        
        // Set frame properties
        // PTS baseado no número de samples processados
        audioFrame->pts = m_ffmpeg.audioPts;
        // Incrementar PTS baseado no número de samples de saída (ret)
        // Isso garante que o PTS está sincronizado com o número real de samples
        m_ffmpeg.audioPts += ret;
        
        // Garantir que o PTS não seja negativo
        if (audioFrame->pts < 0) {
            audioFrame->pts = 0;
            m_ffmpeg.audioPts = ret;
        }
        
        // Encode frame
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            break;
        }
        
        ret = avcodec_send_frame(audioCodecCtx, audioFrame);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Falha ao enviar frame de áudio para encoder: " + std::string(errbuf));
            av_packet_free(&pkt);
            break;
        }
        
        while (ret >= 0) {
            ret = avcodec_receive_packet(audioCodecCtx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
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
            if (pkt->dts > pkt->pts) {
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

bool HTTPTSStreamer::muxPacket(void* packet)
{
    if (!packet || !m_active) {
        return false;
    }
    
    AVPacket* pkt = static_cast<AVPacket*>(packet);
    AVFormatContext* formatCtx = static_cast<AVFormatContext*>(m_ffmpeg.formatCtx);
    
    if (!formatCtx || !pkt) {
        return false;
    }
    
    // Usar av_interleaved_write_frame para garantir sincronização correta
    // Mas com flush imediato após cada pacote
    int ret = av_interleaved_write_frame(formatCtx, pkt);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Falha ao escrever pacote MPEG-TS: " + std::string(errbuf));
        return false;
    }
    
    // Flush imediato do AVIO context para forçar envio aos clientes
    // Isso é crítico para reduzir latência
    AVIOContext* pb = formatCtx->pb;
    if (pb && pb->buffer_size > 0) {
        avio_flush(pb);
    }
    
    return true;
}

int HTTPTSStreamer::writeToClients(const uint8_t* buf, int buf_size)
{
    if (!buf || buf_size <= 0) {
        return -1;
    }
    
    std::lock_guard<std::mutex> lock(m_outputMutex);
    
    // Send to all connected clients
    auto it = m_clientSockets.begin();
    while (it != m_clientSockets.end()) {
        int clientFd = *it;
        ssize_t sent = send(clientFd, buf, buf_size, MSG_NOSIGNAL);
        
        if (sent < 0 || sent != buf_size) {
            // Client disconnected or error - remove from list
            close(clientFd);
            it = m_clientSockets.erase(it);
            m_clientCount--;
        } else {
            ++it;
        }
    }
    
    return buf_size;
}

