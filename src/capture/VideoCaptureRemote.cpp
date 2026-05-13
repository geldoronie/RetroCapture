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

void VideoCaptureRemote::setInterpolationMode(InterpolationMode mode)
{
    if (m_interpolationMode.load() == mode) return;
    m_interpolationMode.store(mode);

    // Hot-switch hygiene: linear mode keeps a blend of the previous and
    // next frame stuck on screen until the queue advances, so flipping
    // back and forth between Linear / Nearest / Off while a stream is
    // running leaves the previous mode's last frame as a static ghost
    // ("o primeiro frame que chegou está estático na frente" — exactly
    // that). Drop any queued frames and forget the last consumed one so
    // the next captureLatestFrame call rebuilds state from scratch in
    // the new mode. m_streamAnchored is reset too, so the decode thread
    // re-establishes the wall-clock anchor on the next decoded frame —
    // the previous mode's anchor may have drifted relative to where the
    // new mode wants to start.
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_frameQueue.clear();
        m_lastConsumed = QueuedFrame{};
        m_blendBuffer.clear();
    }
    m_streamAnchored.store(false);
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
    // PTS-anchored release with per-refresh linear interpolation. The
    // decoder tagged each queued frame with the wall-clock time at
    // which it should appear on screen. We drain frames whose target
    // has passed (current frame becomes m_lastConsumed), then LERP
    // between m_lastConsumed and queue.front() based on how far the
    // current wall clock is between the two — so each refresh of a
    // 144 Hz panel showing a 60 fps stream gets a unique intermediate
    // image instead of an alternating 2-or-3-refresh hold. That
    // eliminates the 3:2 pulldown judder that's otherwise inherent to
    // any non-integer stream:refresh ratio, and lets the server
    // transmit at whatever rate it wants without the client needing
    // a "magic" matched fps.
    const int64_t nowWallUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch()).count();
    while (!m_frameQueue.empty() && m_frameQueue.front().targetWallUs <= nowWallUs)
    {
        m_lastConsumed = std::move(m_frameQueue.front());
        m_frameQueue.pop_front();
        ++m_statConsumed;
    }
    if (m_lastConsumed.rgb.empty())
    {
        return false;
    }

    // Default: hand out the cached frame as-is. The Linear / Nearest
    // branches below may override with a blended or temporally-closer
    // frame. Frame.data is non-const uint8_t* in the IVideoCapture
    // contract; cast away const where we point at a queued frame's
    // buffer because the downstream FrameProcessor only reads.
    uint8_t *outData = m_lastConsumed.rgb.data();
    size_t outSize   = m_lastConsumed.rgb.size();

    const InterpolationMode mode = m_interpolationMode.load();
    if (mode != InterpolationMode::Off && !m_frameQueue.empty())
    {
        const QueuedFrame &next = m_frameQueue.front();
        // Both endpoints must have matching dims for a meaningful blend
        // / comparison. Skip interpolation across a resize so the
        // texture upload path doesn't see a half-resized frame.
        if (next.width == m_lastConsumed.width &&
            next.height == m_lastConsumed.height &&
            next.rgb.size() == m_lastConsumed.rgb.size() &&
            next.targetWallUs > m_lastConsumed.targetWallUs)
        {
            const int64_t span = next.targetWallUs - m_lastConsumed.targetWallUs;
            int64_t pos = nowWallUs - m_lastConsumed.targetWallUs;
            if (pos < 0) pos = 0;
            if (pos > span) pos = span;

            if (mode == InterpolationMode::Linear)
            {
                // 0..256 fixed-point weight for the *next* frame; the
                // previous frame gets (256 - tFp). Autovectorises to
                // AVX2 byte-blend on x86_64.
                const int tFp     = static_cast<int>((pos * 256) / span);
                const int oneMinus = 256 - tFp;
                if (tFp > 0 && tFp < 256)
                {
                    if (m_blendBuffer.size() != m_lastConsumed.rgb.size())
                    {
                        m_blendBuffer.assign(m_lastConsumed.rgb.size(), 0);
                    }
                    const uint8_t *a = m_lastConsumed.rgb.data();
                    const uint8_t *b = next.rgb.data();
                    uint8_t *o       = m_blendBuffer.data();
                    const size_t n   = m_blendBuffer.size();
                    for (size_t i = 0; i < n; ++i)
                    {
                        o[i] = static_cast<uint8_t>((a[i] * oneMinus + b[i] * tFp) >> 8);
                    }
                    outData = m_blendBuffer.data();
                    outSize = m_blendBuffer.size();
                }
                else if (tFp >= 256)
                {
                    outData = const_cast<uint8_t *>(next.rgb.data());
                    outSize = next.rgb.size();
                }
                // tFp == 0 → stays on m_lastConsumed.
            }
            else // InterpolationMode::Nearest
            {
                // Pick whichever target is closer to now in time.
                if (pos * 2 >= span)
                {
                    outData = const_cast<uint8_t *>(next.rgb.data());
                    outSize = next.rgb.size();
                }
                // Otherwise stays on m_lastConsumed.
            }
        }
    }

    frame.data   = outData;
    frame.size   = outSize;
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
        // Reconnect path: when the previous av_read_frame tore the
        // connection down (server stop / network blip) we land here
        // with m_formatCtx == nullptr. Try to (re)open the input,
        // back off on failure, keep trying until the application
        // asks us to stop.
        if (!m_formatCtx)
        {
            cleanupDecoder(); // belt-and-braces: drop any half-init state
            if (!initDecoder())
            {
                LOG_WARN("VideoCaptureRemote: reconnect to " + m_url + " failed, retrying in 2 s");
                cleanupDecoder();
                for (int i = 0; i < 20 && m_decodeRunning.load(); ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }
            // Fresh stream: reset the PTS anchor and clear any stale
            // frames that were sitting in the queue from before the
            // disconnect. Otherwise the new stream's PTS=0 frame would
            // be timed against the old anchor and either play in the
            // past or sit far in the future.
            m_streamAnchored.store(false);
            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                m_frameQueue.clear();
            }
            rgbW = 0; rgbH = 0; rgbBuf.clear();
            LOG_INFO("VideoCaptureRemote: reconnected to " + m_url);
        }

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
            LOG_WARN("VideoCaptureRemote::decodeLoop — av_read_frame: " + std::string(errBuf) +
                     " — tearing down and waiting to reconnect");
            // Tear down so the top of the outer loop reopens. Don't
            // exit the thread — server is allowed to come back.
            cleanupDecoder();
            continue;
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

            // Capture the frame's PTS in stream timebase units BEFORE
            // unref'ing. Used together with the per-stream anchor below
            // to compute when this frame should appear on screen.
            const int64_t framePtsTicks = (frame->pts != AV_NOPTS_VALUE) ? frame->pts : 0;
            av_frame_unref(frame);

            // Establish the wall-clock anchor on the first decoded frame.
            // From here on, every frame's target display time is computed
            // off this anchor — so the consumer can hold each frame on
            // screen for its exact stream-defined duration, rather than
            // showing whatever-is-in-the-queue at every 16 ms poll, which
            // produces visibly irregular hold times when the network /
            // decoder delivers frames in bursts.
            const int64_t nowWallUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t targetWallUs = nowWallUs;
            if (!m_streamAnchored.load())
            {
                m_streamAnchored.store(true);
                m_streamStartWallUs = nowWallUs;
                m_firstPtsTicks     = framePtsTicks;
                if (m_formatCtx && m_videoStreamIdx >= 0)
                {
                    const AVRational tb = m_formatCtx->streams[m_videoStreamIdx]->time_base;
                    m_streamTimebaseSecs = av_q2d(tb);
                }
                if (m_streamTimebaseSecs <= 0.0) m_streamTimebaseSecs = 1.0 / 90000.0;
            }
            else
            {
                const int64_t deltaTicks = framePtsTicks - m_firstPtsTicks;
                const int64_t deltaUs    = static_cast<int64_t>(static_cast<double>(deltaTicks) * m_streamTimebaseSecs * 1e6);
                targetWallUs = m_streamStartWallUs + deltaUs;
                // If we've fallen far behind (deltaUs in the past), snap
                // the anchor forward so we don't accumulate latency
                // forever when the stream pauses and resumes.
                if (targetWallUs + 500'000 < nowWallUs)
                {
                    m_streamStartWallUs = nowWallUs;
                    m_firstPtsTicks     = framePtsTicks;
                    targetWallUs        = nowWallUs;
                }
            }

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
                qf.width        = static_cast<uint32_t>(dstW);
                qf.height       = static_cast<uint32_t>(dstH);
                qf.targetWallUs = targetWallUs;
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
