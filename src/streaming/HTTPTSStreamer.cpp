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
        return false;
    }

    // Now we need to push the raw frame data and width and height to the Frame Queue
    std::shared_ptr<std::vector<uint8_t>> frameData = std::make_shared<std::vector<uint8_t>>(data, data + width * height * 3);
    std::lock_guard<std::mutex> lock(m_frameQueueMutex);
    m_frameQueue.emplace(frameData, std::make_pair(width, height));
    LOG_INFO("Frame pushed to queue (width: " + std::to_string(width) + ", height: " + std::to_string(height) + ")");

    return true;
}

bool HTTPTSStreamer::pushAudio(const int16_t *samples, size_t sampleCount)
{
    if (!samples || !m_active || sampleCount == 0)
    {
        return false;
    }

    // Now we need to push the audio samples to the Audio Queue
    std::shared_ptr<std::vector<int16_t>> audioSamples = std::make_shared<std::vector<int16_t>>(samples, samples + sampleCount);
    std::lock_guard<std::mutex> lock(m_audioQueueMutex);
    m_audioQueue.emplace(audioSamples, std::make_pair(m_audioSampleRate, m_audioChannelsCount));
    LOG_INFO("Audio samples pushed to queue (sampleRate: " + std::to_string(m_audioSampleRate) + ", channels: " + std::to_string(m_audioChannelsCount) + ", sampleCount: " + std::to_string(sampleCount) + ")");

    return true;
}

bool HTTPTSStreamer::start()
{
    m_running = true;
    m_active = true;

    m_serverThread = std::thread(&HTTPTSStreamer::serverThread, this);
    m_encodingThread = std::thread(&HTTPTSStreamer::encodingThread, this);

    // is here where i start the web server?
    if (!initializeFFmpeg())
    {
        LOG_ERROR("Failed to initialize FFmpeg");
        return false;
    }

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
    if (m_serverThread.joinable())
    {
        m_serverThread.join();
    }
    if (m_encodingThread.joinable())
    {
        m_encodingThread.join();
    }
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
}

int HTTPTSStreamer::writeToClients(const uint8_t *buf, int buf_size)
{
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
    const AVCodec *codec = avcodec_find_encoder_by_name(m_videoCodecName.c_str());
    if (!codec)
    {
        LOG_ERROR("Video codec " + m_videoCodecName + " not found");
        return false;
    }
    m_videoCodecContext = avcodec_alloc_context3(codec);
    if (!m_videoCodecContext)
    {
        LOG_ERROR("Failed to allocate video codec context");
        return false;
    }
    m_videoCodecContext->codec_id = codec->id;
    m_videoCodecContext->codec_type = AVMEDIA_TYPE_VIDEO;
    m_videoCodecContext->width = m_width;
    m_videoCodecContext->height = m_height;
    m_videoCodecContext->time_base = {1, static_cast<int>(m_fps)};
    m_videoCodecContext->framerate = {static_cast<int>(m_fps), 1};
    m_videoCodecContext->gop_size = 1;
    m_videoCodecContext->max_b_frames = 0;
    m_videoCodecContext->pix_fmt = AV_PIX_FMT_YUV420P;
    m_videoCodecContext->bit_rate = m_videoBitrate;
    m_videoCodecContext->codec = codec;
    m_videoCodecContext->thread_count = 4;
    return true;
}

bool HTTPTSStreamer::initializeAudioCodec()
{
    // Initialize the audio codec
    const AVCodec *codec = avcodec_find_encoder_by_name(m_audioCodecName.c_str());
    if (!codec)
    {
        LOG_ERROR("Audio codec " + m_audioCodecName + " not found");
        return false;
    }
    m_audioCodecContext = avcodec_alloc_context3(codec);
    if (!m_audioCodecContext)
    {
        LOG_ERROR("Failed to allocate audio codec context");
        return false;
    }
    m_audioCodecContext->codec_id = codec->id;
    m_audioCodecContext->codec_type = AVMEDIA_TYPE_AUDIO;
    m_audioCodecContext->sample_rate = m_audioSampleRate;
    m_audioCodecContext->ch_layout.nb_channels = m_audioChannelsCount;
    m_audioCodecContext->ch_layout.u.map = nullptr;
    m_audioCodecContext->sample_fmt = AV_SAMPLE_FMT_S16;
    m_audioCodecContext->bit_rate = m_audioBitrate;
    m_audioCodecContext->codec = codec;
    m_audioCodecContext->thread_count = 4;
    return true;
}

bool HTTPTSStreamer::initializeMuxers()
{
    // Initialize the muxers
    m_muxerContext = avformat_alloc_context();
    if (!m_muxerContext)
    {
        LOG_ERROR("Failed to allocate muxer context");
        return false;
    }
    m_muxerContext->oformat = av_guess_format("mpegts", nullptr, nullptr);
    if (!m_muxerContext->oformat)
    {
        LOG_ERROR("Failed to guess muxer format");
        return false;
    }
    m_muxerContext->url = strdup("pipe:");
    if (!m_muxerContext->url)
    {
        LOG_ERROR("Failed to allocate muxer URL");
        return false;
    }

    return true;
}

void HTTPTSStreamer::cleanupFFmpeg()
{
    if (!m_videoCodecContext)
    {
        return;
    }
    if (!m_audioCodecContext)
    {
        return;
    }
    if (!m_muxerContext)
    {
        return;
    }
    avcodec_free_context(&m_videoCodecContext);
    avcodec_free_context(&m_audioCodecContext);
    avformat_free_context(&m_muxerContext);
    m_videoCodecContext = nullptr;
    m_audioCodecContext = nullptr;
    m_muxerContext = nullptr;
}

void HTTPTSStreamer::flushCodecs()
{
    if (!m_videoCodecContext)
    {
        return;
    }
    if (!m_audioCodecContext)
    {
        return;
    }
    avcodec_flush_buffers(m_videoCodecContext);
    avcodec_flush_buffers(m_audioCodecContext);
    if (!m_muxerContext)
    {
        return;
    }
    avformat_flush(m_muxerContext);
}

bool HTTPTSStreamer::encodeVideoFrame(const uint8_t *rgbData, uint32_t width, uint32_t height)
{
    if (!rgbData || !m_active || width == 0 || height == 0)
    {
        return false;
    }

    // Now we need to encode the video frame to the encoded video queue
    std::lock_guard<std::mutex> lock(m_frameQueueMutex);
    m_frameQueue.emplace(std::make_shared<std::vector<uint8_t>>(rgbData, rgbData + width * height * 3), std::make_pair(width, height));
    LOG_INFO("Video frame pushed to queue (width: " + std::to_string(width) + ", height: " + std::to_string(height) + ")");

    // Now we need to encode the video frame
    return true;
}

bool HTTPTSStreamer::encodeAudioFrame(const int16_t *samples, size_t sampleCount)
{
    if (!samples || !m_active || sampleCount == 0)
    {
        return false;
    }

    // Now we need to encode the audio frame to the encoded audio queue
    std::lock_guard<std::mutex> lock(m_audioQueueMutex);
    m_audioQueue.emplace(std::make_shared<std::vector<int16_t>>(samples, samples + sampleCount), std::make_pair(m_audioSampleRate, m_audioChannelsCount));
    LOG_INFO("Audio frame pushed to queue (sampleRate: " + std::to_string(m_audioSampleRate) + ", channels: " + std::to_string(m_audioChannelsCount) + ", sampleCount: " + std::to_string(sampleCount) + ")");

    // Now we need to encode the audio frame
    return true;
}

bool HTTPTSStreamer::muxPacket(void *packet)
{
    if (!packet)
    {
        return false;
    }
    if (!m_muxerContext)
    {
        return false;
    }
    if (av_interleaved_write_frame(m_muxerContext, (AVPacket *)packet) < 0)
    {
        LOG_ERROR("Failed to write packet");
        return false;
    }

    return true;
}

void HTTPTSStreamer::serverThread()
{
    while (m_running)
    {
        // Now we need to dequeue the frame and audio from the frame and audio queues
        printf("Server thread running\n");
        usleep(100000);
    }
}

void HTTPTSStreamer::encodingThread()
{
    while (m_running)
    {
        // Now we need to dequeue the frame and audio from the frame and audio queues
        std::pair<std::shared_ptr<std::vector<uint8_t>>, std::pair<uint32_t, uint32_t>> frame;
        std::pair<std::shared_ptr<std::vector<int16_t>>, std::pair<uint32_t, uint32_t>> audio;
        {
            std::lock_guard<std::mutex> lock(m_frameQueueMutex);
            if (!m_frameQueue.empty())
            {
                frame = m_frameQueue.front();
                m_frameQueue.pop();
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_audioQueueMutex);
            if (!m_audioQueue.empty())
            {
                audio = m_audioQueue.front();
                m_audioQueue.pop();
            }
        }

        // Now we need to encode the frame and audio
        if (frame.first && frame.second.first > 0 && frame.second.second > 0)
        {
            if (!encodeVideoFrame(frame.first->data(), frame.second.first, frame.second.second))
            {
                LOG_ERROR("Failed to encode video frame");
            }
        }
        if (audio.first && audio.second.first > 0 && audio.second.second > 0)
        {
            if (!encodeAudioFrame(audio.first->data(), audio.second.first))
            {
                LOG_ERROR("Failed to encode audio frame");
            }
        }

        printf("Encoding thread running\n");
        usleep(100000);
    }
}
