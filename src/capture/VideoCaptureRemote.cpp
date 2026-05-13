#include "VideoCaptureRemote.h"
#include "../utils/Logger.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cstring>

VideoCaptureRemote::VideoCaptureRemote() = default;

VideoCaptureRemote::~VideoCaptureRemote()
{
    close();
}

bool VideoCaptureRemote::open(const std::string &url)
{
    if (m_open.load())
    {
        LOG_WARN("VideoCaptureRemote::open — already open, closing previous connection first");
        close();
    }

    if (url.empty())
    {
        LOG_ERROR("VideoCaptureRemote::open — empty URL");
        return false;
    }

    m_url = url;

    // Strip trailing slash from base URL so the appended /raw lands cleanly.
    if (!m_url.empty() && m_url.back() == '/')
    {
        m_url.pop_back();
    }

    LOG_INFO("VideoCaptureRemote: connecting to remote base URL " + m_url);

    if (!initDecoder())
    {
        cleanupDecoder();
        return false;
    }

    m_open.store(true);
    return true;
}

bool VideoCaptureRemote::initDecoder()
{
    const std::string rawUrl = m_url + "/raw";

    // FFmpeg networking init — idempotent, cheap to call.
    avformat_network_init();

    // Hint that we want low-latency probing. Without these flags FFmpeg may
    // buffer up several seconds of input before deciding what stream params
    // it has.
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "probesize",    "32768",        0); // bytes
    av_dict_set(&opts, "analyzeduration", "500000",    0); // microseconds (0.5s)
    // Note: deliberately NOT setting "fflags: nobuffer" here — without
    // FFmpeg's internal packet buffer, av_read_frame returns whatever the
    // socket has *right now*, which on a bursty TCP stream means several
    // packets arrive together and then a quiet period. Letting FFmpeg
    // buffer ~2-3 packets internally smooths consumption without
    // meaningfully hurting the live-stream latency floor.
    av_dict_set(&opts, "timeout",      "5000000",      0); // 5s read timeout (microseconds)

    int rc = avformat_open_input(&m_formatCtx, rawUrl.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (rc < 0)
    {
        char errBuf[256] = {0};
        av_strerror(rc, errBuf, sizeof(errBuf));
        LOG_ERROR("VideoCaptureRemote: avformat_open_input failed for " + rawUrl + ": " + errBuf);
        return false;
    }

    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0)
    {
        LOG_ERROR("VideoCaptureRemote: avformat_find_stream_info failed");
        return false;
    }

    m_videoStreamIdx = -1;
    for (unsigned int i = 0; i < m_formatCtx->nb_streams; ++i)
    {
        if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            m_videoStreamIdx = static_cast<int>(i);
            break;
        }
    }
    if (m_videoStreamIdx < 0)
    {
        LOG_ERROR("VideoCaptureRemote: no video stream in remote feed");
        return false;
    }

    AVCodecParameters *codecPar = m_formatCtx->streams[m_videoStreamIdx]->codecpar;
    const AVCodec *codec = avcodec_find_decoder(codecPar->codec_id);
    if (!codec)
    {
        LOG_ERROR("VideoCaptureRemote: no decoder for codec id " + std::to_string(codecPar->codec_id));
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx)
    {
        LOG_ERROR("VideoCaptureRemote: avcodec_alloc_context3 failed");
        return false;
    }
    if (avcodec_parameters_to_context(m_codecCtx, codecPar) < 0)
    {
        LOG_ERROR("VideoCaptureRemote: avcodec_parameters_to_context failed");
        return false;
    }
    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0)
    {
        LOG_ERROR("VideoCaptureRemote: avcodec_open2 failed");
        return false;
    }

    m_width  = static_cast<uint32_t>(m_codecCtx->width);
    m_height = static_cast<uint32_t>(m_codecCtx->height);
    m_pixelFormat = 0; // signals "RGB24" to FrameProcessor

    LOG_INFO("VideoCaptureRemote: decoder ready — " + std::to_string(m_width) + "x" + std::to_string(m_height) +
             " codec=" + std::string(codec->name));

    return true;
}

void VideoCaptureRemote::cleanupDecoder()
{
    if (m_swsCtx)
    {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    if (m_codecCtx)
    {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    if (m_formatCtx)
    {
        avformat_close_input(&m_formatCtx);
        m_formatCtx = nullptr;
    }
    m_videoStreamIdx = -1;
}

void VideoCaptureRemote::close()
{
    stopCapture();
    cleanupDecoder();
    m_open.store(false);
    m_url.clear();
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_frameQueue.clear();
        m_lastConsumed = QueuedFrame{};
        m_hasFrame.store(false);
    }
}

bool VideoCaptureRemote::setFormat(uint32_t, uint32_t, uint32_t)
{
    // The remote server dictates the resolution. captureFrame returns frames
    // at whatever size the decoder produces; the upstream FrameProcessor is
    // already happy with dynamic frame sizes.
    return true;
}

bool VideoCaptureRemote::setFramerate(uint32_t)
{
    // Same idea — server-controlled.
    return true;
}

bool VideoCaptureRemote::startCapture()
{
    if (!m_open.load())
    {
        LOG_ERROR("VideoCaptureRemote::startCapture — not open");
        return false;
    }
    if (m_decodeRunning.load())
    {
        return true;
    }
    m_decodeRunning.store(true);
    m_decodeThread = std::thread(&VideoCaptureRemote::decodeLoop, this);
    return true;
}

void VideoCaptureRemote::stopCapture()
{
    if (!m_decodeRunning.load())
    {
        return;
    }
    m_decodeRunning.store(false);
    if (m_decodeThread.joinable())
    {
        m_decodeThread.join();
    }
}

void VideoCaptureRemote::setTargetResolution(uint32_t width, uint32_t height)
{
    // Atomic stores — picked up by the decode thread on the next frame.
    // sws_getCachedContext detects the size change and rebuilds.
    m_targetWidth.store(width);
    m_targetHeight.store(height);
}

bool VideoCaptureRemote::captureFrame(Frame &frame)
{
    return captureLatestFrame(frame);
}

bool VideoCaptureRemote::captureLatestFrame(Frame &frame)
{
    std::lock_guard<std::mutex> lock(m_frameMutex);
    // Pop the oldest queued frame and remember it as the "last consumed"
    // so subsequent reads with an empty queue still return something
    // sensible (the previous frame) instead of false — keeping the main
    // loop's render path hot avoids the dummy/black flash that single-
    // slot semantics produced under bursty decode timing.
    if (!m_frameQueue.empty())
    {
        m_lastConsumed = std::move(m_frameQueue.front());
        m_frameQueue.pop_front();
        ++m_statConsumed;
    }
    if (m_lastConsumed.rgb.empty())
    {
        return false;
    }
    frame.data   = m_lastConsumed.rgb.data();
    frame.size   = m_lastConsumed.rgb.size();
    frame.width  = m_lastConsumed.width;
    frame.height = m_lastConsumed.height;
    frame.format = 0; // RGB24 — FrameProcessor's non-YUYV path
    return true;
}

void VideoCaptureRemote::decodeLoop()
{
    AVPacket *packet = av_packet_alloc();
    AVFrame  *frame  = av_frame_alloc();
    AVFrame  *rgb    = av_frame_alloc();
    if (!packet || !frame || !rgb)
    {
        LOG_ERROR("VideoCaptureRemote::decodeLoop — av_frame_alloc / av_packet_alloc failed");
        if (packet) av_packet_free(&packet);
        if (frame)  av_frame_free(&frame);
        if (rgb)    av_frame_free(&rgb);
        return;
    }

    // RGB scratch buffer attached to the rgb AVFrame; sized lazily once we
    // see the first decoded frame, since the source dimensions may differ
    // from m_codecCtx->width/height after a key-frame parameter set update.
    int rgbW = 0, rgbH = 0;
    std::vector<uint8_t> rgbBuf;

    while (m_decodeRunning.load())
    {
        int rc = av_read_frame(m_formatCtx, packet);
        if (rc < 0)
        {
            if (rc == AVERROR(EAGAIN))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            char errBuf[128] = {0};
            av_strerror(rc, errBuf, sizeof(errBuf));
            LOG_WARN("VideoCaptureRemote::decodeLoop — av_read_frame: " + std::string(errBuf));
            break; // Connection lost / EOF — exit loop.
        }

        if (packet->stream_index != m_videoStreamIdx)
        {
            av_packet_unref(packet);
            continue;
        }

        if (avcodec_send_packet(m_codecCtx, packet) < 0)
        {
            av_packet_unref(packet);
            continue;
        }
        av_packet_unref(packet);

        while (m_decodeRunning.load())
        {
            int r = avcodec_receive_frame(m_codecCtx, frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            if (r < 0)
            {
                LOG_WARN("VideoCaptureRemote::decodeLoop — avcodec_receive_frame failed");
                break;
            }

            const int srcW = frame->width;
            const int srcH = frame->height;
            if (srcW <= 0 || srcH <= 0) { av_frame_unref(frame); continue; }

            // Apply optional rescale target. When the host reports a source
            // resolution via /meta that differs from the encoded /raw size,
            // Application calls setTargetResolution so the shader pipeline
            // sees frames at the host's logical source dims (so e.g. CRT
            // scanline density / NTSC artefact spacing match the look the
            // host renders locally). When unset, pass through at stream size.
            const uint32_t tgtW32 = m_targetWidth.load();
            const uint32_t tgtH32 = m_targetHeight.load();
            const int dstW = (tgtW32 > 0) ? static_cast<int>(tgtW32) : srcW;
            const int dstH = (tgtH32 > 0) ? static_cast<int>(tgtH32) : srcH;

            // (Re)build the sws context if dimensions or pixfmt changed.
            const AVPixelFormat srcFmt = static_cast<AVPixelFormat>(frame->format);
            m_swsCtx = sws_getCachedContext(m_swsCtx,
                                            srcW, srcH, srcFmt,
                                            dstW, dstH, AV_PIX_FMT_RGB24,
                                            SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!m_swsCtx)
            {
                LOG_ERROR("VideoCaptureRemote::decodeLoop — sws_getCachedContext failed");
                av_frame_unref(frame);
                continue;
            }

            if (dstW != rgbW || dstH != rgbH)
            {
                rgbW = dstW;
                rgbH = dstH;
                rgbBuf.assign(static_cast<size_t>(dstW) * static_cast<size_t>(dstH) * 3, 0);
            }

            // Write rows in reverse (bottom-up) so the resulting RGB buffer
            // matches the orientation the rest of the capture pipeline
            // expects from V4L2 / DirectShow sources — without this, the
            // image renders upside-down on screen.
            uint8_t *dstSlices[1] = { rgbBuf.data() + static_cast<size_t>(dstH - 1) * static_cast<size_t>(dstW) * 3 };
            int dstStrides[1]     = { -(dstW * 3) };
            sws_scale(m_swsCtx, frame->data, frame->linesize, 0, srcH, dstSlices, dstStrides);
            av_frame_unref(frame);

            // Push the new frame onto the bounded queue. Drop the oldest
            // if we've already buffered kMaxQueued — keeps latency bounded
            // while absorbing the TCP / decoder bursts that would
            // otherwise show up as visible stuttering with a single-slot
            // buffer.
            bool droppedOldest = false;
            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                QueuedFrame qf;
                qf.rgb.assign(rgbBuf.begin(), rgbBuf.end());
                qf.width  = static_cast<uint32_t>(dstW);
                qf.height = static_cast<uint32_t>(dstH);
                if (m_frameQueue.size() >= kMaxQueued)
                {
                    m_frameQueue.pop_front();
                    droppedOldest = true;
                }
                m_frameQueue.push_back(std::move(qf));
            }
            m_width  = static_cast<uint32_t>(dstW);
            m_height = static_cast<uint32_t>(dstH);
            m_hasFrame.store(true);

            // Periodic decode stats so we can spot rate mismatches in the
            // log — produce/drop counts over the last second.
            ++m_statProduced;
            if (droppedOldest) ++m_statDropped;
            auto nowTs = std::chrono::steady_clock::now();
            if (m_statStart.time_since_epoch().count() == 0) m_statStart = nowTs;
            if (std::chrono::duration_cast<std::chrono::seconds>(nowTs - m_statStart).count() >= 1)
            {
                LOG_INFO("VideoCaptureRemote: decoded=" + std::to_string(m_statProduced) +
                         "/s consumed=" + std::to_string(m_statConsumed) +
                         "/s drops=" + std::to_string(m_statDropped) +
                         " queueDepth=" + std::to_string(m_frameQueue.size()));
                m_statProduced = m_statConsumed = m_statDropped = 0;
                m_statStart = nowTs;
            }
        }
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&rgb);
}
