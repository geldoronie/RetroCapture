#include "VideoCaptureRemote.h"
#include "../utils/Logger.h"
#include "../audio/IAudioPlayback.h"
#ifdef __linux__
#include "../audio/AudioPlaybackPulse.h"
#elif defined(_WIN32)
#include "../audio/AudioPlaybackWASAPI.h"
#endif

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include "../utils/FFmpegCompat.h"
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

    // Allocate format context up-front so we can install the interrupt
    // callback before avformat_open_input is called. Without the
    // callback ffmpeg's blocking I/O sits in recv()/connect() until its
    // 5 s timeout, ignoring our stopCapture()/close() — the decode loop
    // ends up keeping the socket and the decode thread alive for up to
    // 5 s after Disconnect, which is what produced 'decoded=60/s
    // consumed=0/s drops=60' in the logs after the user hit Disconnect.
    m_formatCtx = avformat_alloc_context();
    if (!m_formatCtx)
    {
        LOG_ERROR("VideoCaptureRemote: avformat_alloc_context failed");
        return false;
    }
    static_cast<AVFormatContext *>(m_formatCtx)->interrupt_callback.callback =
        [](void *opaque) -> int {
            auto *self = static_cast<VideoCaptureRemote *>(opaque);
            // Returning non-zero asks ffmpeg to abort the current
            // blocking I/O call with AVERROR_EXIT. Only abort when
            // stopCapture()/close() has explicitly asked us to —
            // checking m_decodeRunning instead would trip during the
            // initial open() (worker thread hasn't started yet, so
            // the flag is still false) and avformat_open_input would
            // return Immediate exit right away.
            return self && self->m_decodeAborted.load() ? 1 : 0;
        };
    static_cast<AVFormatContext *>(m_formatCtx)->interrupt_callback.opaque = this;

    // Clear any stale abort flag from a previous teardown so this
    // initDecoder pass starts with permission to block in I/O.
    m_decodeAborted.store(false);

    // Probing budget: must see at least one packet of each expected
    // stream so audio is detected, but every byte the probe consumes
    // becomes a packet sitting in the demuxer's internal buffer when
    // the decode loop starts. Too generous a probe means the decoder
    // chews through that buffer at non-real-time rates ('decoded=83/s'
    // catch-up the user observed) and the queue fills before the
    // wall-clock-driven consumer can drain it.
    //
    // 512 KB / 2.5 s is the smallest window that reliably catches the
    // AAC ADTS codec config on a mid-join (#67). The earlier 256 KB /
    // 1 s budget was hitting the 'Audio: aac, 0 channels: unspecified
    // sample format' probe race: when a fresh client lands inside an
    // MPEG-TS pack that doesn't carry the next PMT inside the first
    // ~700 ms of stream, avformat_find_stream_info gives up before
    // seeing enough AAC frames to determine channels/format. The
    // larger budget guarantees at least one full PAT/PMT cycle plus
    // several AAC packets, which is enough to populate codecpar
    // reliably; the extra ~1 s of catch-up is absorbed by the queue's
    // drop-oldest policy already exercised on every reconnect.
    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "probesize",       "524288",  0); // 512 KB
    av_dict_set(&opts, "analyzeduration", "2500000", 0); // 2.5 s
    // Note: deliberately NOT setting "fflags: nobuffer" here — without
    // FFmpeg's internal packet buffer, av_read_frame returns whatever the
    // socket has *right now*, which on a bursty TCP stream means several
    // packets arrive together and then a quiet period. Letting FFmpeg
    // buffer ~2-3 packets internally smooths consumption without
    // meaningfully hurting the live-stream latency floor.
    av_dict_set(&opts, "timeout",      "5000000",      0); // 5s read timeout (microseconds)

    // #49 Phase 3 — when the host has set a password on /raw, the
    // client must present the bearer token (sha256 hex of the
    // password). FFmpeg's HTTP demuxer concatenates the value of the
    // "headers" option onto its outbound request line, so we hand it
    // a single Authorization header here. Empty token == no auth
    // configured, omit the header entirely so the host's
    // open-by-default path still works.
    if (!m_authToken.empty())
    {
        const std::string hdr = "Authorization: Bearer " + m_authToken + "\r\n";
        av_dict_set(&opts, "headers", hdr.c_str(), 0);
    }

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
    // Reject the open when probe didn't resolve video dimensions. The
    // demuxer is allowed to surface a stream that's still being
    // analyzed (the FFmpeg log line is 'Could not find codec
    // parameters ... none: unspecified size'). If we proceed with
    // width/height = 0, m_width/m_height are stuck at zero for the
    // whole session and the consumer dead-locks at
    //   decoded=N/s consumed=0/s drops=N queueDepth=20
    // because nothing downstream can route 0x0 frames. Better to fail
    // initDecoder and let the outer reconnect loop try again — by the
    // time the next attempt fires, the upstream MPEG-TS is usually
    // past the partial-PMT region that caused the first probe to
    // give up.
    if (codecPar->width <= 0 || codecPar->height <= 0)
    {
        LOG_WARN("VideoCaptureRemote: probe returned " +
                 std::to_string(codecPar->width) + "x" +
                 std::to_string(codecPar->height) +
                 " for video stream — rejecting and forcing reconnect");
        return false;
    }
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

    // Audio decoder + playback. The /raw stream always carries audio
    // (see HTTPTSStreamer::pushAudio) so we expect to find an audio
    // stream alongside the video; if not, we silently skip and the
    // client plays video only. This keeps the audio path additive —
    // a failure here doesn't break the rest of the remote viewer.
    m_audioStreamIdx = -1;
    for (unsigned int i = 0; i < m_formatCtx->nb_streams; ++i)
    {
        if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            m_audioStreamIdx = static_cast<int>(i);
            break;
        }
    }
    if (m_audioStreamIdx < 0)
    {
        LOG_WARN("VideoCaptureRemote: no audio stream in /raw — playing video only");
    }
    if (m_audioStreamIdx >= 0)
    {
        AVCodecParameters *aPar = m_formatCtx->streams[m_audioStreamIdx]->codecpar;
        const AVCodec *aCodec = avcodec_find_decoder(aPar->codec_id);
        if (!aCodec)
        {
            LOG_WARN("VideoCaptureRemote: no decoder for audio codec_id=" + std::to_string(aPar->codec_id));
        }
        if (aCodec)
        {
            m_audioCodecCtx = avcodec_alloc_context3(aCodec);
            const bool gotCtx    = m_audioCodecCtx != nullptr;
            const bool gotParams = gotCtx && avcodec_parameters_to_context(m_audioCodecCtx, aPar) >= 0;
            const int  openRet   = gotParams ? avcodec_open2(m_audioCodecCtx, aCodec, nullptr) : -1;
            if (gotCtx && gotParams && openRet >= 0)
            {
                // Open the system sink at the decoder's native format
                // (e.g. AAC stereo 44.1 kHz). Shared-mode WASAPI /
                // PulseAudio both auto-resample to whatever the device
                // is configured for.
#ifdef __linux__
                m_audioPlayback = std::make_unique<AudioPlaybackPulse>();
#elif defined(_WIN32)
                m_audioPlayback = std::make_unique<AudioPlaybackWASAPI>();
#endif
                const uint32_t rate     = static_cast<uint32_t>(m_audioCodecCtx->sample_rate);
                // Channel count compat: AVCodecContext.channels was
                // deprecated in FFmpeg 5.1 in favour of ch_layout.nb_channels.
                // Use whichever the build exposes.
#if defined(FF_API_OLD_CHANNEL_LAYOUT) || LIBAVCODEC_VERSION_MAJOR < 60
                const uint32_t channels = static_cast<uint32_t>(m_audioCodecCtx->channels);
#else
                const uint32_t channels = static_cast<uint32_t>(m_audioCodecCtx->ch_layout.nb_channels);
#endif
                if (!m_audioPlayback || !m_audioPlayback->open(rate, channels))
                {
                    LOG_WARN("VideoCaptureRemote: audio playback open failed — continuing video-only");
                    m_audioPlayback.reset();
                }
                else
                {
                    // Resampler: decoder may emit planar floats / s16,
                    // but the sink wants packed float32. swr_convert
                    // handles both layout and sample-format conversions
                    // transparently per audio frame.
                    // Resampler setup with explicit channel layout —
                    // setSwrChannelLayout wrapper was leaving the layout
                    // unset on some ffmpeg builds ('swr_init failed:
                    // Input channel count and layout are unset' in the
                    // user log). swr_alloc_set_opts2 / the input-frame
                    // path takes the layout directly from the codec
                    // context, which is what the decoder actually emits,
                    // so we hand the codec's layout in verbatim and tell
                    // the output to mirror it.
#if FFMPEG_USE_NEW_CHANNEL_LAYOUT
                    int allocRet = swr_alloc_set_opts2(
                        &m_swrCtx,
                        &m_audioCodecCtx->ch_layout,   // out layout
                        AV_SAMPLE_FMT_FLT,             // out fmt
                        static_cast<int>(rate),        // out rate
                        &m_audioCodecCtx->ch_layout,   // in layout
                        m_audioCodecCtx->sample_fmt,   // in fmt
                        m_audioCodecCtx->sample_rate,  // in rate
                        0, nullptr);
                    if (allocRet < 0)
                    {
                        LOG_WARN("VideoCaptureRemote: swr_alloc_set_opts2 failed (" + std::to_string(allocRet) + ") — disabling audio");
                        if (m_swrCtx) swr_free(&m_swrCtx);
                        m_audioPlayback.reset();
                        m_swrCtx = nullptr;
                    }
                    else
#else
                    int64_t layout = av_get_default_channel_layout(static_cast<int>(channels));
                    m_swrCtx = swr_alloc_set_opts(
                        nullptr,
                        layout,                        // out layout
                        AV_SAMPLE_FMT_FLT,             // out fmt
                        static_cast<int>(rate),        // out rate
                        layout,                        // in layout (assume same)
                        m_audioCodecCtx->sample_fmt,   // in fmt
                        m_audioCodecCtx->sample_rate,  // in rate
                        0, nullptr);
                    if (!m_swrCtx)
                    {
                        LOG_WARN("VideoCaptureRemote: swr_alloc_set_opts failed — disabling audio");
                        m_audioPlayback.reset();
                    }
                    else
#endif
                    if (swr_init(m_swrCtx) < 0)
                    {
                        LOG_WARN("VideoCaptureRemote: swr_init failed — disabling audio");
                        swr_free(&m_swrCtx);
                        m_audioPlayback.reset();
                    }
                    else
                    {
                        LOG_INFO("VideoCaptureRemote: audio ready — " + std::to_string(rate) +
                                 " Hz x " + std::to_string(channels) + " ch, codec=" +
                                 std::string(aCodec->name));
                    }
                }
            }
            else
            {
                LOG_WARN("VideoCaptureRemote: audio codec open failed");
                if (m_audioCodecCtx) avcodec_free_context(&m_audioCodecCtx);
            }
        }
    }

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

    // Audio side: same teardown order as video. Close the sink first
    // so the playback thread (WASAPI) exits before we free the codec
    // state it might still be reading PTS from.
    if (m_audioPlayback)
    {
        m_audioPlayback->close();
        m_audioPlayback.reset();
    }
    if (m_swrCtx)
    {
        swr_free(&m_swrCtx);
        m_swrCtx = nullptr;
    }
    if (m_audioCodecCtx)
    {
        avcodec_free_context(&m_audioCodecCtx);
        m_audioCodecCtx = nullptr;
    }
    m_audioStreamIdx = -1;
    m_audioScratch.clear();
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
    // Always set the abort flag — open()'s avformat_open_input path
    // also observes it through the interrupt callback. Setting it before
    // the m_decodeRunning early-return below means a stopCapture() that
    // arrives while open() is still blocking unwinds cleanly even if the
    // worker thread itself hasn't started yet.
    m_decodeAborted.store(true);
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
    m_firstPtsUs.store(0);
    // Flush stale audio so a mode-switch doesn't leak samples from
    // the previous timing window into the new one (would briefly
    // glitch the clock until the buffer drained).
    if (m_audioPlayback) m_audioPlayback->flush();
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
    // A/V sync — audio is the master clock when the remote stream
    // carries audio (the typical case). Audio is significantly more
    // sensitive to timing glitches than video; locking the video
    // gate to the audio output keeps lip-sync correct even when the
    // wall clock drifts relative to the audio device clock (which it
    // will, over minutes-long sessions). Without audio we fall back
    // to wall-clock + anchor.
    int64_t nowWallUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    if (m_audioPlayback && m_audioPlayback->isOpen() && m_streamAnchored.load())
    {
        const int64_t audioPtsAbsUs = m_audioPlayback->getClockUs();
        if (audioPtsAbsUs > 0)
        {
            // audioPtsAbsUs is in the same coordinate the video frame
            // PTS uses (microseconds, stream-origin-relative to 0).
            // Each video frame's targetWallUs was computed as
            //   m_streamStartWallUs + (frame_pts_us - m_firstPtsUs).
            // The audio-equivalent 'now' in wall-clock units is then
            //   m_streamStartWallUs + (audio_pts_us - m_firstPtsUs).
            const int64_t audioRelUs    = audioPtsAbsUs - m_firstPtsUs.load();
            const int64_t audioNowWall  = m_streamStartWallUs + audioRelUs;
            // Sanity gate: only let audio drive the video clock if it's
            // within ±500 ms of wall-clock. If the audio clock has run
            // off (pa_simple_get_latency reports stale, the playback
            // sink hasn't actually started accepting samples, etc.) we'd
            // otherwise hold every video frame back forever and the
            // queue fills (the user-observed
            // 'decoded=60/s consumed=30/s drops=30/s queueDepth=20').
            const int64_t drift = audioNowWall - nowWallUs;
            if (drift > -500'000 && drift < 500'000)
            {
                nowWallUs = audioNowWall;
            }
            else
            {
                static int driftLogCount = 0;
                if (driftLogCount++ < 5)
                {
                    LOG_WARN("VideoCaptureRemote: audio clock drift=" + std::to_string(drift) +
                             "us — falling back to wall clock for A/V sync");
                }
            }
        }
    }

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
                // #58 — capped exponential backoff. A host that's
                // briefly hiccuping (Cloudflare blip, TLS glitch,
                // app restart) reconnects within the first 2 s slot.
                // A host that's offline for the night used to mean
                // the client re-shook hands every 2 s indefinitely;
                // that's a TLS handshake per try, which is rude on
                // laptops and visible on the host's tunnel logs when
                // it comes back. Step the delay up to a 60 s ceiling
                // after a handful of failures so a long outage costs
                // ~1 retry per minute instead of 30.
                static const int kBackoffSecs[] = { 2, 2, 5, 5, 15, 30, 60 };
                static const size_t kBackoffN   = sizeof(kBackoffSecs) / sizeof(kBackoffSecs[0]);
                const uint32_t failures = m_consecutiveReconnectFailures.fetch_add(1) + 1;
                const int      slot     = static_cast<int>(std::min<size_t>(failures - 1, kBackoffN - 1));
                const int      delaySec = kBackoffSecs[slot];

                // After ~30 consecutive failures (~15 min at the
                // ceiling) flip the UI hint so the user understands
                // we're still trying but the host doesn't look like
                // it's coming back on its own. We don't auto-disconnect
                // — the URL stays armed and a brief recovery still
                // grabs the next slot.
                if (failures >= 30 && !m_hostLikelyOffline.load())
                {
                    m_hostLikelyOffline.store(true);
                    LOG_WARN("VideoCaptureRemote: " + std::to_string(failures) +
                             " consecutive reconnect failures — host appears offline");
                }

                LOG_WARN("VideoCaptureRemote: reconnect to " + m_url + " failed (" +
                         std::to_string(failures) + " in a row), retrying in " +
                         std::to_string(delaySec) + " s");
                cleanupDecoder();
                // Sleep in 100 ms slices so a stopCapture() call lands
                // within ~100 ms even if we're 60 s into the wait.
                const int slices = delaySec * 10;
                for (int i = 0; i < slices && m_decodeRunning.load(); ++i)
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
            m_firstPtsUs.store(0);
            if (m_audioPlayback) m_audioPlayback->flush();
            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                m_frameQueue.clear();
            }
            rgbW = 0; rgbH = 0; rgbBuf.clear();
            // Reconnect succeeded — drop backoff state so the next
            // hiccup starts from the 2 s slot again.
            m_consecutiveReconnectFailures.store(0);
            m_hostLikelyOffline.store(false);
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

        // Audio packets: decode, resample to packed float32, push to
        // the playback sink. The sink's getClockUs() is the A/V master
        // clock for the video gating below.
        if (packet->stream_index == m_audioStreamIdx && m_audioCodecCtx && m_audioPlayback)
        {
            if (avcodec_send_packet(m_audioCodecCtx, packet) >= 0)
            {
                AVFrame *aFrame = av_frame_alloc();
                while (aFrame && avcodec_receive_frame(m_audioCodecCtx, aFrame) >= 0)
                {
                    const int nbIn    = aFrame->nb_samples;
                    const int outRate = m_audioCodecCtx->sample_rate;
                    // Resampler may need extra output samples — ask
                    // it how many it will emit for this frame.
                    int64_t maxOut = swr_get_out_samples(m_swrCtx, nbIn);
                    if (maxOut < nbIn) maxOut = nbIn;
                    const int channels =
#if defined(FF_API_OLD_CHANNEL_LAYOUT) || LIBAVCODEC_VERSION_MAJOR < 60
                        m_audioCodecCtx->channels;
#else
                        m_audioCodecCtx->ch_layout.nb_channels;
#endif
                    const size_t neededFloats = static_cast<size_t>(maxOut) * static_cast<size_t>(channels);
                    if (m_audioScratch.size() < neededFloats)
                    {
                        m_audioScratch.assign(neededFloats, 0.0f);
                    }
                    uint8_t *outPlanes[1] = { reinterpret_cast<uint8_t *>(m_audioScratch.data()) };
                    const int produced = swr_convert(m_swrCtx, outPlanes, static_cast<int>(maxOut),
                                                     const_cast<const uint8_t **>(aFrame->data), nbIn);
                    if (produced > 0 && aFrame->pts != AV_NOPTS_VALUE)
                    {
                        // Convert the frame's PTS (stream-tb units)
                        // to absolute stream microseconds. The drift
                        // check below subtracts the video anchor
                        // (m_firstPtsUs) to bring this into the same
                        // coordinate system the video frame
                        // targetWallUs uses, so we keep it absolute
                        // here and let the consumer rebase.
                        //
                        // IMPORTANT: skip submit when aFrame->pts is
                        // AV_NOPTS_VALUE. Earlier code substituted 0,
                        // which on a mid-join anchored the audio
                        // clock at "stream-time 0" while video
                        // anchored at the server's current uptime —
                        // producing the multi-hundred-second drift
                        // documented in #67. A few dropped frames
                        // (~20 ms each at AAC 1024-sample boundaries)
                        // is a much better failure mode than a
                        // corrupted A/V clock.
                        const AVRational tb = m_formatCtx->streams[m_audioStreamIdx]->time_base;
                        const int64_t ptsUs = static_cast<int64_t>(static_cast<double>(aFrame->pts) * av_q2d(tb) * 1e6);
                        m_audioPlayback->submit(m_audioScratch.data(),
                                                static_cast<size_t>(produced),
                                                ptsUs);
                    }
                    av_frame_unref(aFrame);
                }
                if (aFrame) av_frame_free(&aFrame);
            }
            av_packet_unref(packet);
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

            // Top-down orientation matches what the rest of the capture
            // pipeline produces:
            //   - V4L2: FrameProcessor::convertYUYVtoRGB uses a positive
            //     sws stride, so the local YUYV→RGB output is top-down.
            //   - DirectShow: VideoCaptureDS emits RGB24 frames top-down.
            //
            // Earlier code here negated the dst stride to write the
            // buffer bottom-up "to match V4L2", which was based on a
            // wrong reading of the local path. The result was the
            // /raw client showing the picture flipped vertically once
            // the shader pipeline on the client was bypassed — the
            // shader implicitly compensated, so the bug only surfaced
            // after #67's "/raw works with shader off" fix exposed the
            // no-shader rendering path on remote sources. See the user
            // report on #67.
            uint8_t *dstSlices[1] = { rgbBuf.data() };
            int dstStrides[1]     = { static_cast<int>(dstW) * 3 };
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
            // Record arrival time of every decoded frame so the UI
            // overlay can tell a live stream from a stalled one
            // even when m_streamAnchored hasn't been reset yet.
            m_lastFrameAtSteadyUs.store(nowWallUs);
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
                // Also store firstPts in microseconds so the audio path
                // can convert its own absolute PTS into the same
                // stream-origin-relative coordinate system.
                m_firstPtsUs.store(static_cast<int64_t>(static_cast<double>(framePtsTicks) * m_streamTimebaseSecs * 1e6));
            }
            else
            {
                const int64_t deltaTicks = framePtsTicks - m_firstPtsTicks;
                const int64_t deltaUs    = static_cast<int64_t>(static_cast<double>(deltaTicks) * m_streamTimebaseSecs * 1e6);
                targetWallUs = m_streamStartWallUs + deltaUs;
                // Watchdog in both directions: see prior comment for why.
                if (targetWallUs + 500'000 < nowWallUs ||
                    targetWallUs > nowWallUs + 500'000)
                {
                    m_streamStartWallUs = nowWallUs;
                    m_firstPtsTicks     = framePtsTicks;
                    m_firstPtsUs.store(static_cast<int64_t>(static_cast<double>(framePtsTicks) * m_streamTimebaseSecs * 1e6));
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
                LOG_DEBUG("VideoCaptureRemote: decoded=" + std::to_string(m_statProduced) +
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
