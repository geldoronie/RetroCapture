#include "HTTPMJPEGStreamer.h"
#include "../utils/Logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sstream>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

HTTPMJPEGStreamer::HTTPMJPEGStreamer()
{
}

HTTPMJPEGStreamer::~HTTPMJPEGStreamer()
{
    cleanup();
}

bool HTTPMJPEGStreamer::initialize(uint16_t port, uint32_t width, uint32_t height, uint32_t fps)
{
    m_port = port;
    m_width = width;
    m_height = height;
    m_fps = fps;

    LOG_INFO("HTTP MJPEG Streamer inicializado: " + std::to_string(width) + "x" + std::to_string(height) +
             " @ " + std::to_string(fps) + "fps, porta " + std::to_string(port));
    return true;
}

bool HTTPMJPEGStreamer::start()
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
    m_serverThread = std::thread(&HTTPMJPEGStreamer::serverThread, this);

    LOG_INFO("HTTP MJPEG Streamer iniciado na porta " + std::to_string(m_port));
    return true;
}

void HTTPMJPEGStreamer::stop()
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

    LOG_INFO("HTTP MJPEG Streamer parado");
}

bool HTTPMJPEGStreamer::isActive() const
{
    return m_active;
}

bool HTTPMJPEGStreamer::pushFrame(const uint8_t *data, uint32_t width, uint32_t height)
{
    if (!data || !m_active)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_frameMutex);

    // Store latest frame
    size_t frameSize = width * height * 3;
    m_latestFrame.resize(frameSize);
    memcpy(m_latestFrame.data(), data, frameSize);
    m_frameWidth = width;
    m_frameHeight = height;
    m_hasFrame = true;

    return true;
}

std::string HTTPMJPEGStreamer::getStreamUrl() const
{
    return "http://localhost:" + std::to_string(m_port) + "/stream";
}

uint32_t HTTPMJPEGStreamer::getClientCount() const
{
    return m_clientCount.load();
}

void HTTPMJPEGStreamer::cleanup()
{
    stop();
    m_hasFrame = false;
}

void HTTPMJPEGStreamer::serverThread()
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
        std::thread clientThread(&HTTPMJPEGStreamer::handleClient, this, clientFd);
        clientThread.detach();
    }
}

void HTTPMJPEGStreamer::handleClient(int clientFd)
{
    m_clientCount++;
    LOG_INFO("Cliente HTTP MJPEG conectado (total: " + std::to_string(m_clientCount.load()) + ")");

    // Read HTTP request (simple - just read until we get a blank line)
    char buffer[4096];
    ssize_t bytesRead = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
    if (bytesRead <= 0)
    {
        close(clientFd);
        m_clientCount--;
        return;
    }

    buffer[bytesRead] = '\0';

    // Check if it's a GET request for /stream
    std::string request(buffer);
    if (request.find("GET /stream") == std::string::npos &&
        request.find("GET /") == std::string::npos)
    {
        // Send 404
        const char *response = "HTTP/1.1 404 Not Found\r\n\r\n";
        send(clientFd, response, strlen(response), 0);
        close(clientFd);
        m_clientCount--;
        return;
    }

    // Send HTTP headers for MJPEG stream
    std::ostringstream headers;
    headers << "HTTP/1.1 200 OK\r\n";
    headers << "Content-Type: multipart/x-mixed-replace; boundary=--retrocapture\r\n";
    headers << "Connection: keep-alive\r\n";
    headers << "\r\n";

    std::string headerStr = headers.str();
    ssize_t sent = send(clientFd, headerStr.c_str(), headerStr.length(), MSG_NOSIGNAL);
    if (sent < 0)
    {
        close(clientFd);
        m_clientCount--;
        return;
    }

    // Stream frames
    const uint32_t frameDelay = 1000000 / m_fps; // microseconds

    while (m_running)
    {
        std::vector<uint8_t> jpegData;
        uint32_t frameWidth, frameHeight;

        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            if (!m_hasFrame)
            {
                usleep(10000); // 10ms
                continue;
            }

            frameWidth = m_frameWidth;
            frameHeight = m_frameHeight;

            // Encode to JPEG
            if (!encodeJPEG(m_latestFrame.data(), frameWidth, frameHeight, jpegData))
            {
                usleep(10000);
                continue;
            }
        }

        // Send multipart boundary and JPEG data
        std::ostringstream frame;
        frame << "--retrocapture\r\n";
        frame << "Content-Type: image/jpeg\r\n";
        frame << "Content-Length: " << jpegData.size() << "\r\n";
        frame << "\r\n";

        std::string frameHeader = frame.str();

        // Send frame header
        ssize_t sent = send(clientFd, frameHeader.c_str(), frameHeader.length(), MSG_NOSIGNAL);
        if (sent < 0)
        {
            break; // Client disconnected
        }

        // Send JPEG data
        sent = send(clientFd, jpegData.data(), jpegData.size(), MSG_NOSIGNAL);
        if (sent < 0)
        {
            break; // Client disconnected
        }

        // Send boundary terminator
        sent = send(clientFd, "\r\n", 2, MSG_NOSIGNAL);
        if (sent < 0)
        {
            break; // Client disconnected
        }

        usleep(frameDelay);
    }

    close(clientFd);
    m_clientCount--;
    LOG_INFO("Cliente HTTP MJPEG desconectado (total: " + std::to_string(m_clientCount.load()) + ")");
}

bool HTTPMJPEGStreamer::encodeJPEG(const uint8_t *rgbData, uint32_t width, uint32_t height, std::vector<uint8_t> &jpegData)
{
    static AVCodecContext *codecContext = nullptr;
    static SwsContext *swsContext = nullptr;
    static AVFrame *frame = nullptr;
    static AVPacket *packet = nullptr;

    // Initialize encoder on first call
    if (!codecContext)
    {
        const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (!codec)
        {
            LOG_ERROR("MJPEG codec não encontrado");
            return false;
        }

        codecContext = avcodec_alloc_context3(codec);
        if (!codecContext)
        {
            LOG_ERROR("Falha ao alocar codec context");
            return false;
        }

        // Configurar bitrate
        if (m_bitrate > 0)
        {
            // Usar bitrate especificado
            codecContext->bit_rate = m_bitrate;
        }
        else
        {
            // Calcular bitrate baseado na qualidade JPEG (1-100)
            // Qualidade 85 = ~400kbps, qualidade 100 = ~2Mbps, qualidade 50 = ~150kbps
            int quality = m_jpegQuality;
            if (quality < 1)
                quality = 1;
            if (quality > 100)
                quality = 100;
            codecContext->bit_rate = 150000 + (quality - 50) * 25000; // Ajuste linear aproximado
        }
        codecContext->width = width;
        codecContext->height = height;
        codecContext->time_base = {1, static_cast<int>(m_fps)};
        codecContext->framerate = {static_cast<int>(m_fps), 1};
        codecContext->gop_size = 1;     // MJPEG não usa GOP, cada frame é independente
        codecContext->max_b_frames = 0; // MJPEG não suporta B-frames
        codecContext->pix_fmt = AV_PIX_FMT_YUVJ420P;

        // Configurar qualidade JPEG usando AVOption
        char qualityStr[16];
        snprintf(qualityStr, sizeof(qualityStr), "%d", m_jpegQuality);
        av_opt_set(codecContext->priv_data, "q", qualityStr, 0);

        if (avcodec_open2(codecContext, codec, nullptr) < 0)
        {
            LOG_ERROR("Falha ao abrir codec MJPEG");
            avcodec_free_context(&codecContext);
            return false;
        }

        frame = av_frame_alloc();
        if (!frame)
        {
            LOG_ERROR("Falha ao alocar frame");
            avcodec_free_context(&codecContext);
            return false;
        }

        frame->format = codecContext->pix_fmt;
        frame->width = codecContext->width;
        frame->height = codecContext->height;
        if (av_frame_get_buffer(frame, 0) < 0)
        {
            LOG_ERROR("Falha ao alocar buffer do frame");
            av_frame_free(&frame);
            avcodec_free_context(&codecContext);
            return false;
        }

        packet = av_packet_alloc();
        if (!packet)
        {
            LOG_ERROR("Falha ao alocar packet");
            av_frame_free(&frame);
            avcodec_free_context(&codecContext);
            return false;
        }

        // Create SWS context for RGB to YUV conversion
        swsContext = sws_getContext(
            width, height, AV_PIX_FMT_RGB24,
            width, height, AV_PIX_FMT_YUVJ420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (!swsContext)
        {
            LOG_ERROR("Falha ao criar SWS context");
            av_packet_free(&packet);
            av_frame_free(&frame);
            avcodec_free_context(&codecContext);
            return false;
        }
    }

    // Update dimensions if changed
    if (codecContext->width != static_cast<int>(width) || codecContext->height != static_cast<int>(height))
    {
        // Reinitialize codec context with new dimensions
        avcodec_free_context(&codecContext);
        sws_freeContext(swsContext);
        av_frame_free(&frame);
        av_packet_free(&packet);
        codecContext = nullptr;
        return encodeJPEG(rgbData, width, height, jpegData);
    }

    // Convert RGB to YUV
    const uint8_t *srcData[1] = {rgbData};
    int srcLinesize[1] = {static_cast<int>(width * 3)};

    if (av_frame_make_writable(frame) < 0)
    {
        return false;
    }

    sws_scale(swsContext, srcData, srcLinesize, 0, height,
              frame->data, frame->linesize);

    // Incrementar PTS a cada frame para evitar erro "Invalid pts"
    frame->pts = m_frameCount++;

    // Encode frame
    int ret = avcodec_send_frame(codecContext, frame);
    if (ret < 0)
    {
        return false;
    }

    ret = avcodec_receive_packet(codecContext, packet);
    if (ret < 0)
    {
        return false;
    }

    // Copy encoded data
    jpegData.resize(packet->size);
    memcpy(jpegData.data(), packet->data, packet->size);

    av_packet_unref(packet);

    return true;
}
