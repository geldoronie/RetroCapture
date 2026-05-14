#include "MediaEncoder.h"
#include "../utils/Logger.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include "../utils/FFmpegCompat.h"
#include <mutex>

const char *MediaEncoder::hardwareEncoderName(HardwareEncoder h)
{
    switch (h)
    {
        case HardwareEncoder::Auto:     return "Auto";
        case HardwareEncoder::Software: return "Software (libx264)";
        case HardwareEncoder::NVENC:    return "NVIDIA NVENC";
        case HardwareEncoder::VAAPI:    return "VAAPI (Linux)";
        case HardwareEncoder::QSV:      return "Intel Quick Sync";
        case HardwareEncoder::AMF:      return "AMD AMF (Windows)";
    }
    return "?";
}

const char *MediaEncoder::hardwareEncoderCodec(HardwareEncoder h, bool isHEVC)
{
    if (isHEVC)
    {
        switch (h)
        {
            case HardwareEncoder::Auto:     return "";
            case HardwareEncoder::Software: return "libx265";
            case HardwareEncoder::NVENC:    return "hevc_nvenc";
            case HardwareEncoder::VAAPI:    return "hevc_vaapi";
            case HardwareEncoder::QSV:      return "hevc_qsv";
            case HardwareEncoder::AMF:      return "hevc_amf";
        }
        return "";
    }
    switch (h)
    {
        case HardwareEncoder::Auto:     return "";          // resolved at runtime
        case HardwareEncoder::Software: return "libx264";
        case HardwareEncoder::NVENC:    return "h264_nvenc";
        case HardwareEncoder::VAAPI:    return "h264_vaapi";
        case HardwareEncoder::QSV:      return "h264_qsv";
        case HardwareEncoder::AMF:      return "h264_amf";
    }
    return "";
}

std::vector<MediaEncoder::HardwareEncoder> MediaEncoder::detectAvailableEncoders()
{
    static std::vector<HardwareEncoder> cache;
    static std::once_flag once;
    std::call_once(once, [] {
        // Software always available (libx264 is a hard dependency of the build).
        cache.push_back(HardwareEncoder::Software);

        const HardwareEncoder candidates[] = {
            HardwareEncoder::NVENC,
            HardwareEncoder::VAAPI,
            HardwareEncoder::QSV,
            HardwareEncoder::AMF,
        };
        for (HardwareEncoder candidate : candidates)
        {
            const char *h264Name = hardwareEncoderCodec(candidate, false);
            const char *hevcName = hardwareEncoderCodec(candidate, true);
            const bool hasH264 = h264Name && *h264Name && avcodec_find_encoder_by_name(h264Name) != nullptr;
            const bool hasHEVC = hevcName && *hevcName && avcodec_find_encoder_by_name(hevcName) != nullptr;
            // Probe both the H.264 and HEVC builds — a backend is
            // reported as available if either codec is compiled in.
            // (Typically both come from the same ffmpeg library, but
            // some custom builds drop one.) Real availability is
            // confirmed at codec-open time; on failure we fall back to
            // software for that specific codec.
            if (hasH264 || hasHEVC)
            {
                cache.push_back(candidate);
                LOG_INFO(std::string("MediaEncoder: hardware encoder available — ") + hardwareEncoderName(candidate) +
                         " (h264=" + (hasH264 ? "yes" : "no") + ", hevc=" + (hasHEVC ? "yes" : "no") + ")");
            }
        }
    });
    return cache;
}

MediaEncoder::MediaEncoder()
{
}

MediaEncoder::~MediaEncoder()
{
    cleanup();
}

bool MediaEncoder::initialize(const VideoConfig &videoConfig, const AudioConfig &audioConfig, bool forStreaming)
{
    if (m_initialized)
    {
        cleanup();
    }

    m_videoConfig = videoConfig;
    m_audioConfig = audioConfig;
    m_forStreaming = forStreaming;

    if (!initializeVideoCodec())
    {
        LOG_ERROR("MediaEncoder: Failed to initialize video codec");
        cleanup();
        return false;
    }

    if (!initializeAudioCodec())
    {
        LOG_ERROR("MediaEncoder: Failed to initialize audio codec");
        cleanup();
        return false;
    }

    m_initialized = true;
    return true;
}

bool MediaEncoder::initializeVideoCodec()
{
    // Hardware encoder dispatcher. Applies to both H.264 and H.265 —
    // the other codecs in this project (vp8/vp9) have no hardware
    // equivalents we support, they keep the software-only path.
    const bool wantHEVC = (m_videoConfig.codec == "h265" ||
                           m_videoConfig.codec == "libx265" ||
                           m_videoConfig.codec == "hevc");
    const bool wantH264 = (m_videoConfig.codec == "h264" ||
                           m_videoConfig.codec == "libx264");
    if (wantH264 || wantHEVC)
    {
        HardwareEncoder selected = m_videoConfig.hardwareEncoder;
        if (selected == HardwareEncoder::Auto)
        {
            // Walk the same priority list detectAvailableEncoders uses,
            // picking the first hardware backend whose codec for the
            // user-selected family is compiled in. open() may still
            // fail (driver missing etc.) — we fall back to software in
            // that case.
            for (HardwareEncoder candidate : { HardwareEncoder::NVENC,
                                               HardwareEncoder::VAAPI,
                                               HardwareEncoder::QSV,
                                               HardwareEncoder::AMF })
            {
                if (avcodec_find_encoder_by_name(hardwareEncoderCodec(candidate, wantHEVC)))
                {
                    selected = candidate;
                    break;
                }
            }
            if (selected == HardwareEncoder::Auto) selected = HardwareEncoder::Software;
        }

        if (selected != HardwareEncoder::Software)
        {
            if (initializeHardwareVideoCodec(selected))
            {
                m_activeHardwareEncoder = selected;
                LOG_INFO(std::string("MediaEncoder: using hardware encoder — ") + hardwareEncoderName(selected));
                return true;
            }
            LOG_WARN(std::string("MediaEncoder: hardware backend ") + hardwareEncoderName(selected) +
                     " failed to initialize, falling back to software libx264/libx265");
            // Tear down anything the failed HW init may have allocated
            // so the software path below starts from a clean slate.
            if (m_videoCodecContext)
            {
                AVCodecContext *cc = static_cast<AVCodecContext *>(m_videoCodecContext);
                avcodec_free_context(&cc);
                m_videoCodecContext = nullptr;
            }
            if (m_hwFramesCtx)
            {
                AVBufferRef *ref = static_cast<AVBufferRef *>(m_hwFramesCtx);
                av_buffer_unref(&ref);
                m_hwFramesCtx = nullptr;
            }
            if (m_hwDeviceCtx)
            {
                AVBufferRef *ref = static_cast<AVBufferRef *>(m_hwDeviceCtx);
                av_buffer_unref(&ref);
                m_hwDeviceCtx = nullptr;
            }
        }
        m_activeHardwareEncoder = HardwareEncoder::Software;
    }

    // Tentar encontrar codec por nome primeiro, depois por ID
    const AVCodec *codec = nullptr;

    if (m_videoConfig.codec == "h264" || m_videoConfig.codec == "libx264")
    {
        codec = avcodec_find_encoder_by_name("libx264");
        if (!codec)
        {
            codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        }
        if (!codec)
        {
            LOG_ERROR("H.264 codec not found. Make sure libx264 is installed.");
            return false;
        }
    }
    else if (m_videoConfig.codec == "h265" || m_videoConfig.codec == "libx265" || m_videoConfig.codec == "hevc")
    {
        codec = avcodec_find_encoder_by_name("libx265");
        if (!codec)
        {
            codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
        }
        if (!codec)
        {
            LOG_ERROR("H.265 codec not found. Make sure libx265 is installed.");
            return false;
        }
    }
    else if (m_videoConfig.codec == "vp8" || m_videoConfig.codec == "libvpx-vp8")
    {
        codec = avcodec_find_encoder_by_name("libvpx-vp8");
        if (!codec)
        {
            codec = avcodec_find_encoder(AV_CODEC_ID_VP8);
        }
        if (!codec)
        {
            LOG_ERROR("VP8 codec not found. Make sure libvpx is installed.");
            return false;
        }
    }
    else if (m_videoConfig.codec == "vp9" || m_videoConfig.codec == "libvpx-vp9")
    {
        codec = avcodec_find_encoder_by_name("libvpx-vp9");
        if (!codec)
        {
            codec = avcodec_find_encoder(AV_CODEC_ID_VP9);
        }
        if (!codec)
        {
            LOG_ERROR("VP9 codec not found. Make sure libvpx is installed.");
            return false;
        }
    }
    else
    {
        codec = avcodec_find_encoder_by_name(m_videoConfig.codec.c_str());
        if (!codec)
        {
            LOG_ERROR("Video codec " + m_videoConfig.codec + " not found");
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
    codecCtx->width = m_videoConfig.width;
    codecCtx->height = m_videoConfig.height;
    // time_base granular (1/90000 — padrão MPEG) em vez de {1, fps}.
    // Com {1, 60}, frames a 16.667ms davam PTS 0.99998→0 e 1.99998→1
    // (truncamento), gerando frames com PTS duplicado, warnings de
    // non-strictly-monotonic-PTS, e quando frames dropam o gap fica
    // grosseiro o suficiente pra arquivo declarar fps menor que o
    // capturado → vídeo toca em câmera lenta. 90000 dá precisão de ~11µs.
    codecCtx->time_base = {1, 90000};
    codecCtx->framerate = {static_cast<int>(m_videoConfig.fps), 1};
    codecCtx->gop_size = static_cast<int>(m_videoConfig.fps * 2);
    codecCtx->max_b_frames = 0;
    codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    codecCtx->bit_rate = m_videoConfig.bitrate;
    codecCtx->thread_count = 0;
    // FF_THREAD_FRAME parallelises encoding across consecutive frames
    // (each thread works on a different frame) instead of FF_THREAD_SLICE
    // which divides one frame into parallel slices. On a multi-core CPU
    // with libx264 at non-trivial presets the per-frame budget is the
    // bottleneck — slice threading caps how much we can speed up a
    // single frame, but frame threading lets us keep N frames in flight
    // simultaneously and roughly multiply throughput by core count.
    // Adds ~thread_count frames of latency (libx264 holds a few frames
    // in its pipeline), acceptable for a /raw distribution stream where
    // we already buffer ~500 ms on the client.
    codecCtx->thread_type = FF_THREAD_FRAME;

    // Global-header policy depends on use: file recording carries
    // extradata in the container header, but a streaming MPEG-TS client
    // connecting mid-stream never receives that extradata, so VPS/SPS/PPS
    // must travel inline with each keyframe (libx264/libx265
    // 'repeat-headers=1'). HEVC used to always set GLOBAL_HEADER here
    // even for streaming, which produced the truncated VPS / 'Invalid
    // NAL unit 35' artefacts observed on the client.
    if (m_forStreaming)
    {
        // Streaming: no codec sets global header.
        if (codec->id == AV_CODEC_ID_VP8 || codec->id == AV_CODEC_ID_VP9)
        {
            // VP8/VP9 in WebM need extradata in the container, but the
            // streaming pipeline never exposes VPx, so the conservative
            // efeito prático é zero. Mantemos GLOBAL_HEADER por
            // default to GLOBAL_HEADER until someone actually streams VPx.
            codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
    }
    else
    {
        // File recording: every codec uses global header.
        codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Codec-specific options
    AVDictionary *opts = nullptr;
    if (codec->id == AV_CODEC_ID_H264)
    {
        av_dict_set(&opts, "preset", m_videoConfig.preset.c_str(), 0);
        // tune=zerolatency in both modes: it disables x264's internal
        // lookahead / B-frames. For file recording the lookahead would
        // only cost extra CPU per frame (default 40-frame lookahead =
        // ~10% more work) and pushes encoders below realtime at
        // preset >= fast. zerolatency keeps cost predictable.
        av_dict_set(&opts, "tune", "zerolatency", 0);
        av_dict_set(&opts, "profile", m_videoConfig.profile.c_str(), 0);
        int keyint = static_cast<int>(m_videoConfig.fps * 2);
        av_dict_set_int(&opts, "keyint_min", keyint, 0);
        av_dict_set_int(&opts, "keyint", keyint, 0);
        av_dict_set_int(&opts, "rc-lookahead", 0, 0);
        av_dict_set_int(&opts, "scenecut", 0, 0);

        if (m_forStreaming)
        {
            // HTTP-TS streaming: tight vbv caps bitrate variation.
            av_dict_set_int(&opts, "vbv-bufsize", m_videoConfig.bitrate / 10, 0);
            av_dict_set_int(&opts, "repeat-headers", 1, 0);
        }
        else
        {
            // Gravação em arquivo: vbv largo pra rate-control flexível.
            av_dict_set_int(&opts, "vbv-bufsize", m_videoConfig.bitrate * 2, 0);
        }
    }
    else if (codec->id == AV_CODEC_ID_HEVC)
    {
        av_dict_set(&opts, "preset", m_videoConfig.preset.c_str(), 0);
        av_dict_set(&opts, "tune", "zerolatency", 0);
        av_dict_set(&opts, "profile", m_videoConfig.h265Profile.c_str(), 0);
        if (m_videoConfig.h265Level != "auto" && !m_videoConfig.h265Level.empty())
        {
            av_dict_set(&opts, "level-idc", m_videoConfig.h265Level.c_str(), 0);
        }
        int keyint = static_cast<int>(m_videoConfig.fps * 2);
        av_dict_set_int(&opts, "keyint_min", keyint, 0);
        av_dict_set_int(&opts, "keyint", keyint, 0);
        av_dict_set_int(&opts, "rc-lookahead", 0, 0);
        av_dict_set_int(&opts, "scenecut", 0, 0);

        if (m_forStreaming)
        {
            av_dict_set_int(&opts, "vbv-bufsize", m_videoConfig.bitrate / 10, 0);
            // Inline VPS/SPS/PPS at every keyframe — libx265 equivalent
            // of libx264's repeat-headers. Without this, a client that
            // joins the MPEG-TS stream after the first keyframe never
            // receives the parameter sets and reads NAL data as random
            // bytes ('Invalid NAL unit 35', 'Truncating likely oversized
            // VPS', 'PCM bit depth out of range').
            av_dict_set(&opts, "x265-params", "repeat-headers=1:annexb=1", 0);
        }
        else
        {
            av_dict_set_int(&opts, "vbv-bufsize", m_videoConfig.bitrate * 2, 0);
        }
    }
    else if (codec->id == AV_CODEC_ID_VP8)
    {
        av_dict_set_int(&opts, "speed", m_videoConfig.vp8Speed, 0);
        av_dict_set(&opts, "deadline", "realtime", 0);
        av_dict_set_int(&opts, "lag-in-frames", 0, 0);
        int keyint = static_cast<int>(m_videoConfig.fps * 2);
        av_dict_set_int(&opts, "keyint_min", keyint, 0);
        av_dict_set_int(&opts, "keyint_max", keyint, 0);
        av_dict_set_int(&opts, "threads", 0, 0);
    }
    else if (codec->id == AV_CODEC_ID_VP9)
    {
        av_dict_set_int(&opts, "speed", m_videoConfig.vp9Speed, 0);
        av_dict_set(&opts, "deadline", "realtime", 0);
        av_dict_set_int(&opts, "lag-in-frames", 0, 0);
        int keyint = static_cast<int>(m_videoConfig.fps * 2);
        av_dict_set_int(&opts, "keyint_min", keyint, 0);
        av_dict_set_int(&opts, "keyint_max", keyint, 0);
        av_dict_set_int(&opts, "threads", 0, 0);
        av_dict_set_int(&opts, "tile-columns", 2, 0);
    }

    if (avcodec_open2(codecCtx, codec, &opts) < 0)
    {
        LOG_ERROR("Failed to open video codec");
        av_dict_free(&opts);
        avcodec_free_context(&codecCtx);
        return false;
    }
    av_dict_free(&opts);

    m_videoCodecContext = codecCtx;
    m_swsContext = nullptr;
    m_swsSrcWidth = 0;
    m_swsSrcHeight = 0;
    m_swsDstWidth = 0;
    m_swsDstHeight = 0;

    AVFrame *videoFrame = av_frame_alloc();
    if (!videoFrame)
    {
        LOG_ERROR("Failed to allocate video frame");
        avcodec_free_context(&codecCtx);
        m_videoCodecContext = nullptr;
        return false;
    }

    videoFrame->format = AV_PIX_FMT_YUV420P;
    videoFrame->width = m_videoConfig.width;
    videoFrame->height = m_videoConfig.height;
    if (av_frame_get_buffer(videoFrame, 0) < 0)
    {
        LOG_ERROR("Failed to allocate video frame buffer");
        av_frame_free(&videoFrame);
        avcodec_free_context(&codecCtx);
        m_videoCodecContext = nullptr;
        return false;
    }
    m_videoFrame = videoFrame;

    return true;
}

namespace
{
    AVHWDeviceType deviceTypeForBackend(MediaEncoder::HardwareEncoder b)
    {
        switch (b)
        {
            case MediaEncoder::HardwareEncoder::VAAPI: return AV_HWDEVICE_TYPE_VAAPI;
            case MediaEncoder::HardwareEncoder::QSV:   return AV_HWDEVICE_TYPE_QSV;
            case MediaEncoder::HardwareEncoder::AMF:   return AV_HWDEVICE_TYPE_D3D11VA;
            default:                                   return AV_HWDEVICE_TYPE_NONE;
        }
    }

    AVPixelFormat hwPixelFormatForBackend(MediaEncoder::HardwareEncoder b)
    {
        switch (b)
        {
            case MediaEncoder::HardwareEncoder::VAAPI: return AV_PIX_FMT_VAAPI;
            case MediaEncoder::HardwareEncoder::QSV:   return AV_PIX_FMT_QSV;
            case MediaEncoder::HardwareEncoder::AMF:   return AV_PIX_FMT_D3D11;
            default:                                   return AV_PIX_FMT_NV12;
        }
    }
}

bool MediaEncoder::createHardwareContext(HardwareEncoder backend)
{
    // NVENC accepts software-allocated YUV420P frames directly; no need
    // to spin up an AVHWFramesContext. The other backends (VAAPI, QSV,
    // AMF/D3D11VA) require a HW device + HW frames context, with the
    // codec receiving AVFrames whose .data lives in GPU memory.
    if (backend == HardwareEncoder::NVENC) return true;

    AVHWDeviceType type = deviceTypeForBackend(backend);
    if (type == AV_HWDEVICE_TYPE_NONE) return false;

    AVBufferRef *deviceCtx = nullptr;
    int rc = av_hwdevice_ctx_create(&deviceCtx, type, nullptr, nullptr, 0);
    if (rc < 0 || !deviceCtx)
    {
        char errBuf[128] = {0};
        av_strerror(rc, errBuf, sizeof(errBuf));
        LOG_WARN(std::string("MediaEncoder: av_hwdevice_ctx_create failed for ") +
                 hardwareEncoderName(backend) + ": " + errBuf);
        return false;
    }
    m_hwDeviceCtx = deviceCtx;

    AVBufferRef *framesCtxRef = av_hwframe_ctx_alloc(deviceCtx);
    if (!framesCtxRef)
    {
        LOG_WARN("MediaEncoder: av_hwframe_ctx_alloc failed");
        return false;
    }
    AVHWFramesContext *framesCtx = reinterpret_cast<AVHWFramesContext *>(framesCtxRef->data);
    framesCtx->format    = hwPixelFormatForBackend(backend);
    framesCtx->sw_format = AV_PIX_FMT_NV12;
    framesCtx->width     = m_videoConfig.width;
    framesCtx->height    = m_videoConfig.height;
    framesCtx->initial_pool_size = 8; // small pool — keep enough surfaces for the in-flight frame threads
    rc = av_hwframe_ctx_init(framesCtxRef);
    if (rc < 0)
    {
        char errBuf[128] = {0};
        av_strerror(rc, errBuf, sizeof(errBuf));
        LOG_WARN(std::string("MediaEncoder: av_hwframe_ctx_init failed: ") + errBuf);
        av_buffer_unref(&framesCtxRef);
        return false;
    }
    m_hwFramesCtx = framesCtxRef;
    return true;
}

bool MediaEncoder::initializeHardwareVideoCodec(HardwareEncoder backend)
{
    const bool wantHEVC = (m_videoConfig.codec == "h265" ||
                           m_videoConfig.codec == "libx265" ||
                           m_videoConfig.codec == "hevc");
    const char *codecName = hardwareEncoderCodec(backend, wantHEVC);
    const AVCodec *codec = avcodec_find_encoder_by_name(codecName);
    if (!codec)
    {
        LOG_WARN(std::string("MediaEncoder: codec ") + codecName + " not present in this ffmpeg build");
        return false;
    }

    if (!createHardwareContext(backend))
    {
        return false;
    }

    AVCodecContext *codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx)
    {
        LOG_ERROR("MediaEncoder: avcodec_alloc_context3 failed for hardware codec");
        return false;
    }
    m_videoCodecContext = codecCtx;

    codecCtx->codec_id   = codec->id;
    codecCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    codecCtx->width      = m_videoConfig.width;
    codecCtx->height     = m_videoConfig.height;
    codecCtx->time_base  = {1, 90000};
    codecCtx->framerate  = {static_cast<int>(m_videoConfig.fps), 1};
    codecCtx->gop_size   = static_cast<int>(m_videoConfig.fps * 2);
    codecCtx->max_b_frames = 0;
    codecCtx->bit_rate   = m_videoConfig.bitrate;
    codecCtx->thread_count = 1; // hardware encoders manage their own parallelism

    if (backend == HardwareEncoder::NVENC)
    {
        // NVENC accepts a regular software YUV420P input frame.
        codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    }
    else
    {
        // VAAPI / QSV / AMF require their hardware-surface pixel format
        // and a populated hw_frames_ctx.
        codecCtx->pix_fmt = hwPixelFormatForBackend(backend);
        codecCtx->hw_frames_ctx = av_buffer_ref(static_cast<AVBufferRef *>(m_hwFramesCtx));
        if (!codecCtx->hw_frames_ctx)
        {
            LOG_ERROR("MediaEncoder: failed to attach hw_frames_ctx to codec");
            return false;
        }
    }

    if (m_forStreaming == false || codec->id == AV_CODEC_ID_HEVC)
    {
        codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Profile selection: pin to Main for both H.264 and HEVC. Every
    // hardware encoder we target supports Main; consumer AMD VAAPI
    // builds frequently lack High encoding entrypoints (the user-visible
    // signature is "No usable encoding entrypoint found for profile
    // VAProfileH264High"). If Main also fails we let the outer
    // dispatcher fall back to the software encoder for that codec.
    codecCtx->profile = wantHEVC ? FF_PROFILE_HEVC_MAIN : FF_PROFILE_H264_MAIN;

    AVDictionary *opts = nullptr;
    // Per-backend tuning. hwPreset from the UI overrides the default
    // when non-empty; meaning differs per backend (NVENC preset / VAAPI
    // rc_mode / QSV preset / AMF quality).
    const std::string &userPreset = m_videoConfig.hwPreset;
    if (backend == HardwareEncoder::NVENC)
    {
        av_dict_set(&opts, "preset", userPreset.empty() ? "p4" : userPreset.c_str(), 0);
        av_dict_set(&opts, "tune",   "ll",  0);          // low-latency
        av_dict_set(&opts, "rc",     "cbr", 0);
        av_dict_set_int(&opts, "zerolatency", 1, 0);
        av_dict_set_int(&opts, "bf",          0, 0);
        // hevc_nvenc/h264_nvenc: emit VPS/SPS/PPS in front of every IDR
        // so MPEG-TS clients that join mid-stream get the parameter
        // sets without needing the muxer's extradata. Without this the
        // client sees keyframes with no headers and decodes garbage.
        av_dict_set_int(&opts, "forced-idr", 1, 0);
    }
    else if (backend == HardwareEncoder::VAAPI)
    {
        av_dict_set(&opts, "rc_mode", userPreset.empty() ? "CBR" : userPreset.c_str(), 0);
        // low_power=1 is unsupported on a chunk of AMD parts — let the
        // driver pick the entry path instead.
        av_dict_set_int(&opts, "idr_interval", static_cast<int>(m_videoConfig.fps * 2), 0);
        // hevc_vaapi / h264_vaapi emit headers inline by default when
        // every IDR is forced (vs. only first keyframe). No additional
        // option needed beyond idr_interval; mid-stream join works.
    }
    else if (backend == HardwareEncoder::QSV)
    {
        av_dict_set(&opts, "preset", userPreset.empty() ? "veryfast" : userPreset.c_str(), 0);
        av_dict_set_int(&opts, "look_ahead", 0, 0);
        // QSV's 'idr_interval' is in seconds and inserts inline headers
        // at every IDR. 0 = once per GOP (default) is sufficient.
    }
    else if (backend == HardwareEncoder::AMF)
    {
        av_dict_set(&opts, "usage",   "transcoding", 0);
        av_dict_set(&opts, "quality", userPreset.empty() ? "speed" : userPreset.c_str(), 0);
        av_dict_set(&opts, "rc",      "cbr",       0);
        // header_insertion_mode=idr → emit VPS/SPS/PPS at every IDR
        // frame (default 'none' only emits once and leaves mid-stream
        // joiners without parameter sets).
        av_dict_set(&opts, "header_insertion_mode", "idr", 0);
    }

    int openRet = avcodec_open2(codecCtx, codec, &opts);
    av_dict_free(&opts);
    if (openRet < 0)
    {
        char errBuf[256] = {0};
        av_strerror(openRet, errBuf, sizeof(errBuf));
        LOG_WARN(std::string("MediaEncoder: avcodec_open2 failed for ") + codecName + ": " + errBuf);
        return false;
    }
    LOG_INFO(std::string("MediaEncoder: ") + codecName + " opened with profile Main");

    AVFrame *swFrame = av_frame_alloc();
    if (!swFrame)
    {
        LOG_ERROR("MediaEncoder: failed to allocate software AVFrame");
        return false;
    }
    // The software-side frame we fill with NV12/YUV420P pixel data — for
    // NVENC this is the frame we feed directly; for VAAPI/QSV/AMF it
    // becomes the upload source via av_hwframe_transfer_data.
    swFrame->format = (backend == HardwareEncoder::NVENC) ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_NV12;
    swFrame->width  = m_videoConfig.width;
    swFrame->height = m_videoConfig.height;
    if (av_frame_get_buffer(swFrame, 0) < 0)
    {
        LOG_ERROR("MediaEncoder: failed to allocate sw frame buffer");
        av_frame_free(&swFrame);
        return false;
    }
    m_videoFrame = swFrame;

    if (backend != HardwareEncoder::NVENC)
    {
        AVFrame *hwFrame = av_frame_alloc();
        if (!hwFrame)
        {
            LOG_ERROR("MediaEncoder: failed to allocate hw AVFrame");
            return false;
        }
        if (av_hwframe_get_buffer(static_cast<AVBufferRef *>(m_hwFramesCtx), hwFrame, 0) < 0)
        {
            LOG_ERROR("MediaEncoder: av_hwframe_get_buffer failed");
            av_frame_free(&hwFrame);
            return false;
        }
        m_hwVideoFrame = hwFrame;
    }

    return true;
}

bool MediaEncoder::initializeAudioCodec()
{
    const AVCodec *codec = nullptr;

    if (m_audioConfig.codec == "aac")
    {
        codec = avcodec_find_encoder_by_name("libfdk_aac");
        if (!codec)
        {
            codec = avcodec_find_encoder_by_name("aac");
        }
        if (!codec)
        {
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
        codec = avcodec_find_encoder_by_name(m_audioConfig.codec.c_str());
        if (!codec)
        {
            LOG_ERROR("Audio codec " + m_audioConfig.codec + " not found");
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
    codecCtx->sample_rate = m_audioConfig.sampleRate;
    FFmpegCompat::setChannelLayout(codecCtx, m_audioConfig.channels);
    codecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    codecCtx->bit_rate = m_audioConfig.bitrate;
    codecCtx->thread_count = 1; // AAC encoding is typically single-threaded
    codecCtx->time_base = {1, static_cast<int>(m_audioConfig.sampleRate)};

    // Configure AAC-specific options for better quality
    AVDictionary *opts = nullptr;
    if (codec->id == AV_CODEC_ID_AAC)
    {
        // Use high quality profile for better audio quality
        av_dict_set(&opts, "profile", "aac_low", 0);
        // Ensure proper bitrate control
        if (m_audioConfig.bitrate > 0)
        {
            av_dict_set_int(&opts, "b", static_cast<int64_t>(m_audioConfig.bitrate), 0);
        }
    }
    else if (codec->id == AV_CODEC_ID_AAC_LATM || 
             (codec->name && std::string(codec->name).find("fdk") != std::string::npos))
    {
        // libfdk_aac specific options
        av_dict_set(&opts, "profile", "aac_low", 0);
        if (m_audioConfig.bitrate > 0)
        {
            av_dict_set_int(&opts, "b", static_cast<int64_t>(m_audioConfig.bitrate), 0);
        }
    }

    if (avcodec_open2(codecCtx, codec, &opts) < 0)
    {
        LOG_ERROR("Failed to open audio codec");
        av_dict_free(&opts);
        avcodec_free_context(&codecCtx);
        return false;
    }
    av_dict_free(&opts);

    m_audioCodecContext = codecCtx;

    SwrContext *swrCtx = swr_alloc();
    if (!swrCtx)
    {
        LOG_ERROR("Failed to allocate SWR context");
        avcodec_free_context(&codecCtx);
        m_audioCodecContext = nullptr;
        return false;
    }

    #if LIBAVCODEC_VERSION_MAJOR >= 59
    AVChannelLayout inChLayout, outChLayout;
    av_channel_layout_default(&inChLayout, m_audioConfig.channels);
    av_channel_layout_default(&outChLayout, m_audioConfig.channels);

    av_opt_set_chlayout(swrCtx, "in_chlayout", &inChLayout, 0);
    av_opt_set_int(swrCtx, "in_sample_rate", static_cast<int>(m_audioConfig.sampleRate), 0);
    av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    av_opt_set_chlayout(swrCtx, "out_chlayout", &outChLayout, 0);
    av_opt_set_int(swrCtx, "out_sample_rate", static_cast<int>(m_audioConfig.sampleRate), 0);
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
    #else
    av_opt_set_int(swrCtx, "in_channel_layout", av_get_default_channel_layout(m_audioConfig.channels), 0);
    av_opt_set_int(swrCtx, "in_sample_rate", static_cast<int>(m_audioConfig.sampleRate), 0);
    av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);

    av_opt_set_int(swrCtx, "out_channel_layout", av_get_default_channel_layout(m_audioConfig.channels), 0);
    av_opt_set_int(swrCtx, "out_sample_rate", static_cast<int>(m_audioConfig.sampleRate), 0);
    av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);

    if (swr_init(swrCtx) < 0)
    {
        LOG_ERROR("Failed to initialize SWR context");
        swr_free(&swrCtx);
        avcodec_free_context(&codecCtx);
        m_audioCodecContext = nullptr;
        return false;
    }
    #endif
    m_swrContext = swrCtx;

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
    FFmpegCompat::setFrameChannelLayout(audioFrame, m_audioConfig.channels);
    audioFrame->sample_rate = m_audioConfig.sampleRate;
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

bool MediaEncoder::convertRGBToYUV(const uint8_t *rgbData, uint32_t width, uint32_t height, void *videoFrame)
{
    if (!rgbData || !videoFrame || width == 0 || height == 0)
    {
        return false;
    }

    AVFrame *frame = static_cast<AVFrame *>(videoFrame);
    AVCodecContext *codecCtx = static_cast<AVCodecContext *>(m_videoCodecContext);

    if (!frame || !codecCtx)
    {
        return false;
    }

    uint32_t dstWidth = m_videoConfig.width;
    uint32_t dstHeight = m_videoConfig.height;

    if (dstWidth == 0 || dstHeight == 0)
    {
        LOG_ERROR("convertRGBToYUV: Invalid destination dimensions");
        return false;
    }

    if (width != dstWidth || height != dstHeight)
    {
        static int logCount = 0;
        if (logCount < 1)
        {
            LOG_WARN("convertRGBToYUV: Resizing " + std::to_string(width) + "x" + std::to_string(height) +
                     " to " + std::to_string(dstWidth) + "x" + std::to_string(dstHeight));
            logCount++;
        }
    }

    // Hardware backends that need NV12 (VAAPI/QSV/AMF upload via NV12
    // surfaces) set frame->format = NV12; software / NVENC keep YUV420P.
    // We honour whatever the frame asks for so the same code path covers
    // both — the sws context is rebuilt if any of source dims / dest dims
    // / dest format changes.
    AVPixelFormat dstFormat = static_cast<AVPixelFormat>(frame->format);
    if (dstFormat != AV_PIX_FMT_YUV420P && dstFormat != AV_PIX_FMT_NV12)
    {
        dstFormat = AV_PIX_FMT_YUV420P;
    }

    SwsContext *swsCtx = static_cast<SwsContext *>(m_swsContext);
    if (!swsCtx || m_swsSrcWidth != width || m_swsSrcHeight != height ||
        m_swsDstWidth != dstWidth || m_swsDstHeight != dstHeight ||
        static_cast<int>(m_swsDstFormat) != static_cast<int>(dstFormat))
    {
        if (swsCtx)
        {
            sws_freeContext(swsCtx);
        }

        swsCtx = sws_getContext(
            width, height, AV_PIX_FMT_RGB24,
            dstWidth, dstHeight, dstFormat,
            SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (!swsCtx)
        {
            LOG_ERROR("Failed to create SwsContext");
            return false;
        }

        m_swsContext = swsCtx;
        m_swsSrcWidth = width;
        m_swsSrcHeight = height;
        m_swsDstWidth = dstWidth;
        m_swsDstHeight = dstHeight;
        m_swsDstFormat = static_cast<int>(dstFormat);
    }

    if (av_frame_make_writable(frame) < 0)
    {
        return false;
    }

    const uint8_t *srcData[1] = {rgbData};
    int srcLinesize[1] = {static_cast<int>(width * 3)};

    int ret = sws_scale(swsCtx, srcData, srcLinesize, 0, height, frame->data, frame->linesize);
    if (ret < 0)
    {
        LOG_ERROR("sws_scale failed: " + std::to_string(ret));
        return false;
    }

    if (ret != static_cast<int>(dstHeight))
    {
        LOG_ERROR("sws_scale: expected " + std::to_string(dstHeight) + " lines, got " + std::to_string(ret));
        return false;
    }

    return true;
}

bool MediaEncoder::convertInt16ToFloatPlanar(const int16_t *samples, size_t sampleCount, void *audioFrame, size_t outputSamples)
{
    if (!samples || !audioFrame || sampleCount == 0 || outputSamples == 0)
    {
        return false;
    }

    SwrContext *swrCtx = static_cast<SwrContext *>(m_swrContext);
    AVFrame *frame = static_cast<AVFrame *>(audioFrame);

    if (!swrCtx || !frame)
    {
        LOG_ERROR("convertInt16ToFloatPlanar: swrCtx or frame is null");
        return false;
    }

    if (av_frame_make_writable(frame) < 0)
    {
        LOG_ERROR("convertInt16ToFloatPlanar: av_frame_make_writable failed");
        return false;
    }

    // Verify input sample count matches expected
    const size_t expectedInputSamples = outputSamples * m_audioConfig.channels;
    if (sampleCount != expectedInputSamples)
    {
        LOG_ERROR("convertInt16ToFloatPlanar: sample count mismatch - got " + 
                  std::to_string(sampleCount) + ", expected " + std::to_string(expectedInputSamples));
        return false;
    }

    // Prepare input data for swr_convert
    // Input is S16 interleaved: [L0, R0, L1, R1, ...]
    // For swr_convert, we need to pass the number of samples per channel (not total samples)
    const uint8_t *srcData[1] = {reinterpret_cast<const uint8_t *>(samples)};
    const int inputSamples = static_cast<int>(outputSamples); // Number of samples per channel

    // Use swr_convert to convert S16 interleaved to FLTP planar
    // This is the correct and tested way to do format conversion in FFmpeg
    // swr_convert expects: (dst, dst_count, src, src_count)
    // where src_count is the number of samples per channel in the input
    int ret = swr_convert(swrCtx, frame->data, static_cast<int>(outputSamples),
                          srcData, inputSamples);
    
    if (ret < 0)
    {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("swr_convert failed: " + std::to_string(ret) + " (" + std::string(errbuf) + ")");
        return false;
    }

    // For format-only conversion (same sample rate), swr_convert should return exactly outputSamples
    // If it returns fewer, check for configuration issues
    if (ret != static_cast<int>(outputSamples))
    {
        // Check resampler delay - this should be minimal for format-only conversion
        int64_t delay = swr_get_delay(swrCtx, m_audioConfig.sampleRate);
        if (delay > 0)
        {
            LOG_WARN("swr_convert returned " + std::to_string(ret) + " samples, expected " + 
                     std::to_string(outputSamples) + " (resampler delay: " + std::to_string(delay) + 
                     ") - this may cause audio issues");
        }
        
        // For format conversion only, we should get exactly what we request
        // If not, there's a configuration problem
        if (ret == 0)
        {
            // Resampler needs more input - this shouldn't happen for format-only conversion
            LOG_WARN("swr_convert returned 0 samples - resampler needs more input (unexpected for format conversion)");
            return false;
        }
        
        // Use what we got, but log a warning
        frame->nb_samples = ret;
        return true;
    }

    frame->nb_samples = outputSamples;
    return true;
}

int64_t MediaEncoder::calculateVideoPTS(int64_t captureTimestampUs)
{
    // Store first timestamp for reference
    if (!m_firstVideoTimestampSet)
    {
        m_firstVideoTimestampUs = captureTimestampUs;
        m_firstVideoTimestampSet = true;
        m_videoFrameCountForPTS = 0;
    }

    AVCodecContext *codecCtx = static_cast<AVCodecContext *>(m_videoCodecContext);
    if (!codecCtx)
    {
        return 0;
    }

    // CRITICAL: Calculate PTS based on actual timestamps, not frame count
    // Using frame count assumes frames arrive at exactly the configured FPS,
    // which may not be true. Using real timestamps ensures video speed matches reality.
    // This is the same approach used for audio (samples_processed / sample_rate)
    AVRational timeBase = codecCtx->time_base;
    int64_t calculatedPTS = 0;
    
    // Calculate PTS based on actual elapsed time since first frame
    // This ensures video speed matches real time, regardless of actual capture FPS
    int64_t relativeTimeUs = captureTimestampUs - m_firstVideoTimestampUs;
    double relativeTimeSeconds = static_cast<double>(relativeTimeUs) / 1000000.0;
    calculatedPTS = static_cast<int64_t>(relativeTimeSeconds * timeBase.den / timeBase.num);
    
    // Increment frame count for statistics (but don't use it for PTS calculation)
    m_videoFrameCountForPTS++;

    // Log PTS calculation occasionally for debugging
    static int ptsLogCounter = 0;
    ptsLogCounter++;
    if (ptsLogCounter == 1 || ptsLogCounter % 300 == 0) // Every 5 seconds at 60fps
    {
        LOG_INFO("MediaEncoder: Video PTS - calculated: " + std::to_string(calculatedPTS) + 
                 ", relativeTimeUs: " + std::to_string(relativeTimeUs) + 
                 ", relativeTimeSeconds: " + std::to_string(relativeTimeSeconds) +
                 ", timeBase: " + std::to_string(timeBase.num) + "/" + std::to_string(timeBase.den) +
                 ", fps: " + std::to_string(m_videoConfig.fps));
    }

    // Use timestamp-based PTS directly - let MediaMuxer handle monotonicity
    // This ensures correct speed based on actual timestamps
    // MediaMuxer will ensure PTS doesn't go backwards when writing packets

    return calculatedPTS;
}

bool MediaEncoder::encodeAudio(const int16_t *samples, size_t sampleCount,
                               int64_t captureTimestampUs, std::vector<EncodedPacket> &packets)
{
    if (!samples || !m_initialized || sampleCount == 0)
    {
        return false;
    }

    AVCodecContext *codecCtx = static_cast<AVCodecContext *>(m_audioCodecContext);
    AVFrame *audioFrame = static_cast<AVFrame *>(m_audioFrame);

    if (!codecCtx || !audioFrame)
    {
        return false;
    }

    // Acumular samples — sem reset/gap detection. PTS é sample-count
    // based; contar tudo que entra mantém audio smooth e o drain no
    // RecordingManager garante que nada drope antes de chegar aqui.
    {
        std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
        if (m_audioAccumulator.empty())
        {
            m_audioAccumulatorStartCaptureTsUs = captureTimestampUs;
            m_audioAccumulatorTsValid = true;
        }
        m_audioAccumulator.insert(m_audioAccumulator.end(), samples, samples + sampleCount);
    }

    const int samplesPerFrame = codecCtx->frame_size;
    if (samplesPerFrame <= 0)
    {
        LOG_ERROR("MediaEncoder: Invalid frame_size: " + std::to_string(samplesPerFrame));
        return false;
    }

    const int totalSamplesNeeded = samplesPerFrame * m_audioConfig.channels;
    bool processedAny = false;
    
    // Log accumulator status occasionally for debugging
    static int debugLogCounter = 0;
    debugLogCounter++;
    if (debugLogCounter == 1 || debugLogCounter % 100 == 0)
    {
        std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
        LOG_INFO("MediaEncoder: Audio accumulator - size: " + std::to_string(m_audioAccumulator.size()) + 
                 ", needed: " + std::to_string(totalSamplesNeeded) + 
                 ", frame_size: " + std::to_string(samplesPerFrame) +
                 ", channels: " + std::to_string(m_audioConfig.channels));
    }

    // Processar frames completos enquanto houver samples suficientes
    // CRITICAL: Maintain a minimum buffer to avoid gaps in audio
    // If we process a frame and the accumulator would be too low, wait for more samples
    const int MIN_BUFFER_AFTER_PROCESSING = totalSamplesNeeded; // Keep at least one frame worth of samples
    
    while (true)
    {
        std::vector<int16_t> samplesToProcess;

        {
            std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
            // Check if we have enough samples for a frame AND enough to maintain minimum buffer
            if (static_cast<int>(m_audioAccumulator.size()) < totalSamplesNeeded + MIN_BUFFER_AFTER_PROCESSING)
            {
                break; // Não há samples suficientes para um frame completo + buffer mínimo
            }

            // Pegar samples para este frame
            samplesToProcess.assign(m_audioAccumulator.begin(), m_audioAccumulator.begin() + totalSamplesNeeded);
            // Don't erase yet - we'll erase after successful conversion
        }

        // Converter int16 para float planar
        // This may return false if resampler needs more input (internal delay)
        // CRITICAL: If conversion fails, we must NOT remove samples from accumulator
        // The samples are still in the accumulator and will be retried when more arrive
        if (!convertInt16ToFloatPlanar(samplesToProcess.data(), totalSamplesNeeded, audioFrame, samplesPerFrame))
        {
            // Resampler needs more input - this is normal
            // The resampler has internal buffering and needs more samples to produce output
            // We keep the samples in the accumulator and will retry when more samples arrive
            // Don't log as error, this is expected behavior
            break;
        }

        // Get actual samples converted (from frame->nb_samples set by convertInt16ToFloatPlanar)
        int actualFrameSamples = static_cast<AVFrame *>(audioFrame)->nb_samples;

        // Log frame processing occasionally for debugging
        static int frameLogCounter = 0;
        frameLogCounter++;
        if (frameLogCounter == 1 || frameLogCounter % 50 == 0)
        {
            LOG_INFO("MediaEncoder: Processing audio frame - actual: " + std::to_string(actualFrameSamples) + 
                     ", expected: " + std::to_string(samplesPerFrame) + 
                     ", input consumed: " + std::to_string(totalSamplesNeeded));
        }

        // Frame timestamp = capture ts do primeiro sample que está saindo
        // agora. Igual o vídeo, anda no wall clock real. Pegar antes de
        // consumir os samples do accumulator.
        int64_t frameTimestampUs = m_audioAccumulatorStartCaptureTsUs;

        // Conversion successful - now we can remove the samples from accumulator
        {
            std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
            size_t sizeBefore = m_audioAccumulator.size();
            m_audioAccumulator.erase(m_audioAccumulator.begin(), m_audioAccumulator.begin() + totalSamplesNeeded);
            size_t sizeAfter = m_audioAccumulator.size();

            // Avança o capture ts do accumulator pra refletir o que sobrou.
            if (m_audioConfig.sampleRate > 0 && m_audioConfig.channels > 0)
            {
                int64_t consumedSamplesPerCh = static_cast<int64_t>(totalSamplesNeeded) /
                                               static_cast<int64_t>(m_audioConfig.channels);
                m_audioAccumulatorStartCaptureTsUs += (consumedSamplesPerCh * 1000000LL) /
                                                       static_cast<int64_t>(m_audioConfig.sampleRate);
            }
            if (m_audioAccumulator.empty())
            {
                m_audioAccumulatorTsValid = false;
            }

            if (frameLogCounter == 1 || frameLogCounter % 50 == 0)
            {
                LOG_INFO("MediaEncoder: After processing - accumulator: " + std::to_string(sizeBefore) +
                         " -> " + std::to_string(sizeAfter) +
                         " (removed " + std::to_string(totalSamplesNeeded) + ")");
            }
        }

        // PTS agora é wall-clock-relative (igual vídeo).
        int64_t calculatedPTS = calculateAudioPTS(frameTimestampUs, actualFrameSamples * m_audioConfig.channels);
        audioFrame->pts = calculatedPTS;
        
        // Increment total samples processed AFTER calculating PTS
        // Use actual samples processed (from frame), not totalSamplesNeeded
        // This ensures PTS progression matches actual audio data
        m_totalAudioSamplesProcessed += (actualFrameSamples * m_audioConfig.channels);
        m_audioFrameCount++;

        // Enviar frame para codec
        int ret = avcodec_send_frame(codecCtx, audioFrame);
        if (ret < 0)
        {
            if (ret == AVERROR(EAGAIN))
            {
                // Codec está cheio, receber pacotes pendentes
                receiveAudioPackets(packets, frameTimestampUs);
                // Tentar novamente
                ret = avcodec_send_frame(codecCtx, audioFrame);
            }
            if (ret < 0 && ret != AVERROR(EAGAIN))
            {
                LOG_ERROR("MediaEncoder: avcodec_send_frame (audio) failed: " + std::to_string(ret));
                break;
            }
        }

        // Receber pacotes gerados
        if (receiveAudioPackets(packets, frameTimestampUs))
        {
            processedAny = true;
        }
    }

    return processedAny;
}

int64_t MediaEncoder::calculateAudioPTS(int64_t captureTimestampUs, size_t /* sampleCount */)
{
    // Sample-count-based PTS: cada output frame avança PTS por
    // exatamente samples_processados/sample_rate. Smooth, monotônico,
    // sem jitter de capture timestamp (pulseaudio entrega chunks com
    // variabilidade de chegada que não reflete tempo real de captura).
    // O alinhamento com wall clock é garantido pelo drain de áudio no
    // RecordingManager — chunks não dropam mais no synchronizer, então
    // a contagem de samples casa com a duração real da gravação.
    if (!m_firstAudioTimestampSet)
    {
        m_firstAudioTimestampUs = captureTimestampUs;
        m_firstAudioTimestampSet = true;
        m_totalAudioSamplesProcessed = 0;
        m_audioFrameCount = 0;
    }

    AVCodecContext *codecCtx = static_cast<AVCodecContext *>(m_audioCodecContext);
    if (!codecCtx)
    {
        return 0;
    }

    AVRational timeBase = codecCtx->time_base;
    int64_t calculatedPTS = 0;
    if (m_audioConfig.sampleRate > 0 && m_audioConfig.channels > 0)
    {
        int64_t samplesPerChannel = m_totalAudioSamplesProcessed / m_audioConfig.channels;
        calculatedPTS = (samplesPerChannel * timeBase.den) / (m_audioConfig.sampleRate * timeBase.num);
    }
    else
    {
        int64_t relativeTimeUs = captureTimestampUs - m_firstAudioTimestampUs;
        if (relativeTimeUs < 0) relativeTimeUs = 0;
        calculatedPTS = (relativeTimeUs * timeBase.den) / (timeBase.num * 1000000LL);
    }

    {
        std::lock_guard<std::mutex> lock(m_ptsMutex);
        if (m_lastAudioFramePTS >= 0 && calculatedPTS <= m_lastAudioFramePTS)
        {
            calculatedPTS = m_lastAudioFramePTS + 1;
            uint64_t total = m_desyncFrameCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (total == 1 || (total % 60) == 0)
            {
                LOG_WARN("MediaEncoder: PTS retrocession on audio frame (total: " +
                         std::to_string(total) + ")");
            }
        }
        m_lastAudioFramePTS = calculatedPTS;
    }

    return calculatedPTS;
}

void MediaEncoder::ensureMonotonicPTS(int64_t &pts, int64_t &dts, bool isVideo)
{
    // -1 representa AV_NOPTS_VALUE
    const int64_t AV_NOPTS_VALUE_LOCAL = -1;

    std::lock_guard<std::mutex> lock(m_ptsMutex);

    if (isVideo)
    {
        // PTS is in DISPLAY order, DTS is in DECODE order. We do NOT
        // force PTS monotonic across packets — that was breaking
        // B-frame display order in slower presets (visible ghost
        // effect). PTS values pass through.
        //
        // DTS still needs to be strictly monotonic, both for muxer
        // sanity and because libavcodec occasionally emits packets
        // with duplicate / regressing DTS (observed under medium
        // preset + higher bitrate). When we bump DTS forward to
        // maintain monotonicity we may push it above the packet's
        // PTS — but MPEG-TS requires PTS >= DTS for every packet
        // ("pts < dts in stream 0" error). When that happens we
        // pull PTS up to match the bumped DTS so the invariant
        // holds. Display order is preserved relative to other
        // packets because DTS monotonicity is preserved.
        if (dts != AV_NOPTS_VALUE_LOCAL)
        {
            if (m_lastVideoDTS >= 0 && dts <= m_lastVideoDTS)
            {
                dts = m_lastVideoDTS + 1;
            }
            m_lastVideoDTS = dts;
        }
        if (pts != AV_NOPTS_VALUE_LOCAL && dts != AV_NOPTS_VALUE_LOCAL && pts < dts)
        {
            pts = dts;
        }
        if (pts != AV_NOPTS_VALUE_LOCAL)
        {
            m_lastVideoPTS = pts;
        }
    }
    else
    {
        if (pts != AV_NOPTS_VALUE_LOCAL)
        {
            if (m_lastAudioPTS >= 0 && pts <= m_lastAudioPTS)
            {
                pts = m_lastAudioPTS + 1;
                uint64_t total = m_desyncFrameCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (total == 1 || (total % 60) == 0)
                {
                    LOG_WARN("MediaEncoder: PTS retrocession on audio frame (muxer-level, total: " +
                             std::to_string(total) + ")");
                }
            }
            m_lastAudioPTS = pts;
        }
        if (dts != AV_NOPTS_VALUE_LOCAL)
        {
            if (m_lastAudioDTS >= 0 && dts <= m_lastAudioDTS)
            {
                dts = m_lastAudioDTS + 1;
            }
            m_lastAudioDTS = dts;
        }
        if (pts != AV_NOPTS_VALUE_LOCAL && dts != AV_NOPTS_VALUE_LOCAL && dts > pts)
        {
            dts = pts;
            m_lastAudioDTS = dts;
        }
    }
}

bool MediaEncoder::encodeVideo(const uint8_t *rgbData, uint32_t width, uint32_t height,
                               int64_t captureTimestampUs, std::vector<EncodedPacket> &packets)
{
    if (!rgbData || !m_initialized || width == 0 || height == 0)
    {
        return false;
    }

    size_t expectedSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
    if (expectedSize == 0 || expectedSize > 100 * 1024 * 1024)
    {
        LOG_ERROR("MediaEncoder: Invalid frame dimensions: " + std::to_string(width) + "x" + std::to_string(height));
        return false;
    }

    AVCodecContext *codecCtx = static_cast<AVCodecContext *>(m_videoCodecContext);
    AVFrame *videoFrame = static_cast<AVFrame *>(m_videoFrame);

    if (!codecCtx || !videoFrame)
    {
        return false;
    }

    if (!convertRGBToYUV(rgbData, width, height, videoFrame))
    {
        LOG_ERROR("MediaEncoder: convertRGBToYUV failed");
        return false;
    }

    int64_t calculatedPTS = calculateVideoPTS(captureTimestampUs);
    videoFrame->pts = calculatedPTS;

    bool forceKeyframe = false;
    if (m_videoFrameCount == 0)
    {
        forceKeyframe = true;
    }
    else
    {
        AVCodecContext *ctx = static_cast<AVCodecContext *>(m_videoCodecContext);
        if (ctx && m_videoFrameCount > 0 && (m_videoFrameCount % (ctx->gop_size / 2) == 0))
        {
            forceKeyframe = true;
        }
    }

    if (forceKeyframe)
    {
        videoFrame->pict_type = AV_PICTURE_TYPE_I;
        FFmpegCompat::setKeyFrame(videoFrame, true);
    }
    m_videoFrameCount++;

    // HW backends that hold pixel data in a GPU surface (VAAPI / QSV /
    // AMF / D3D11) need the software-side NV12 frame uploaded to a
    // hardware surface before the codec can consume it. NVENC and the
    // software path skip this and feed the sw frame directly.
    AVFrame *frameToSend = videoFrame;
    if (m_hwVideoFrame &&
        m_activeHardwareEncoder != HardwareEncoder::Software &&
        m_activeHardwareEncoder != HardwareEncoder::NVENC)
    {
        AVFrame *hwFrame = static_cast<AVFrame *>(m_hwVideoFrame);
        int rc = av_hwframe_transfer_data(hwFrame, videoFrame, 0);
        if (rc < 0)
        {
            char errBuf[128] = {0};
            av_strerror(rc, errBuf, sizeof(errBuf));
            LOG_ERROR(std::string("MediaEncoder: av_hwframe_transfer_data failed: ") + errBuf);
            return false;
        }
        hwFrame->pts        = videoFrame->pts;
        hwFrame->pict_type  = videoFrame->pict_type;
        FFmpegCompat::setKeyFrame(hwFrame, forceKeyframe);
        frameToSend = hwFrame;
    }

    int ret = avcodec_send_frame(codecCtx, frameToSend);
    if (ret < 0)
    {
        if (ret == AVERROR(EAGAIN))
        {
            receiveVideoPackets(packets, captureTimestampUs);
            ret = avcodec_send_frame(codecCtx, frameToSend);
        }
        if (ret < 0 && ret != AVERROR(EAGAIN))
        {
            LOG_ERROR("MediaEncoder: avcodec_send_frame failed");
            return false;
        }
    }
    // Suppress -Wunused for the conditional re-send variable when the HW
    // path is inactive.
    (void)frameToSend;

    return receiveVideoPackets(packets, captureTimestampUs);
}

bool MediaEncoder::receiveVideoPackets(std::vector<EncodedPacket> &packets, int64_t captureTimestampUs)
{
    AVCodecContext *codecCtx = static_cast<AVCodecContext *>(m_videoCodecContext);
    if (!codecCtx)
    {
        return false;
    }

    AVPacket *pkt = av_packet_alloc();
    bool receivedAny = false;

    while (true)
    {
        int ret = avcodec_receive_packet(codecCtx, pkt);
        if (ret < 0)
        {
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                break;
            }
            else
            {
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("MediaEncoder: avcodec_receive_packet failed: " + std::string(errbuf));
                break;
            }
        }

        receivedAny = true;

        if (!pkt->data || pkt->size <= 0 || pkt->size > 10 * 1024 * 1024)
        {
            av_packet_unref(pkt);
            continue;
        }

        EncodedPacket encodedPkt;
        encodedPkt.data.reserve(pkt->size);
        encodedPkt.data.assign(pkt->data, pkt->data + pkt->size);

        if (encodedPkt.data.size() != static_cast<size_t>(pkt->size))
        {
            LOG_ERROR("MediaEncoder: Failed to copy video packet");
            av_packet_unref(pkt);
            continue;
        }

        encodedPkt.pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : -1;
        encodedPkt.dts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : -1;
        encodedPkt.isKeyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        encodedPkt.isVideo = true;
        encodedPkt.captureTimestampUs = captureTimestampUs;

        // Garantir monotonicidade
        if (encodedPkt.pts != -1 && encodedPkt.dts != -1)
        {
            ensureMonotonicPTS(encodedPkt.pts, encodedPkt.dts, true);
        }

        packets.push_back(std::move(encodedPkt));
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return receivedAny;
}

bool MediaEncoder::receiveAudioPackets(std::vector<EncodedPacket> &packets, int64_t captureTimestampUs)
{
    AVCodecContext *codecCtx = static_cast<AVCodecContext *>(m_audioCodecContext);
    if (!codecCtx)
    {
        return false;
    }

    AVPacket *pkt = av_packet_alloc();
    bool receivedAny = false;

    while (true)
    {
        int ret = avcodec_receive_packet(codecCtx, pkt);
        if (ret < 0)
        {
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            {
                break;
            }
            else
            {
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("MediaEncoder: avcodec_receive_packet (audio) failed: " + std::string(errbuf));
                break;
            }
        }

        receivedAny = true;

        if (!pkt->data || pkt->size <= 0 || pkt->size > 10 * 1024 * 1024)
        {
            av_packet_unref(pkt);
            continue;
        }

        EncodedPacket encodedPkt;
        encodedPkt.data.reserve(pkt->size);
        encodedPkt.data.assign(pkt->data, pkt->data + pkt->size);

        if (encodedPkt.data.size() != static_cast<size_t>(pkt->size))
        {
            LOG_ERROR("MediaEncoder: Failed to copy audio packet");
            av_packet_unref(pkt);
            continue;
        }

        encodedPkt.pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : -1;
        encodedPkt.dts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : -1;
        encodedPkt.isKeyframe = false; // Áudio não tem keyframes
        encodedPkt.isVideo = false;
        encodedPkt.captureTimestampUs = captureTimestampUs;

        if (encodedPkt.pts != -1 && encodedPkt.dts != -1)
        {
            ensureMonotonicPTS(encodedPkt.pts, encodedPkt.dts, false);
        }

        packets.push_back(std::move(encodedPkt));
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return receivedAny;
}

void MediaEncoder::flush(std::vector<EncodedPacket> &packets)
{
    if (!m_initialized)
    {
        return;
    }

    AVCodecContext *videoCtx = static_cast<AVCodecContext *>(m_videoCodecContext);
    if (videoCtx)
    {
        avcodec_send_frame(videoCtx, nullptr);
        receiveVideoPackets(packets, 0);
    }

    AVCodecContext *audioCtx = static_cast<AVCodecContext *>(m_audioCodecContext);
    AVFrame *audioFrame = static_cast<AVFrame *>(m_audioFrame);
    if (audioCtx && audioFrame)
    {
        // CRITICAL: Process all remaining samples in accumulator before flushing
        // This ensures no audio is lost at the end of recording
        const int samplesPerFrame = audioCtx->frame_size;
        if (samplesPerFrame > 0)
        {
            const int totalSamplesNeeded = samplesPerFrame * m_audioConfig.channels;
            
            while (true)
            {
                std::vector<int16_t> samplesToProcess;
                int64_t frameTsUs = 0;
                {
                    std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
                    if (static_cast<int>(m_audioAccumulator.size()) < totalSamplesNeeded)
                    {
                        break;
                    }
                    samplesToProcess.assign(m_audioAccumulator.begin(), m_audioAccumulator.begin() + totalSamplesNeeded);
                    frameTsUs = m_audioAccumulatorStartCaptureTsUs;
                }

                if (convertInt16ToFloatPlanar(samplesToProcess.data(), totalSamplesNeeded, audioFrame, samplesPerFrame))
                {
                    int actualFrameSamples = static_cast<AVFrame *>(audioFrame)->nb_samples;

                    int64_t calculatedPTS = calculateAudioPTS(frameTsUs, actualFrameSamples * m_audioConfig.channels);
                    audioFrame->pts = calculatedPTS;
                    m_totalAudioSamplesProcessed += (actualFrameSamples * m_audioConfig.channels);

                    int ret = avcodec_send_frame(audioCtx, audioFrame);
                    if (ret < 0 && ret != AVERROR(EAGAIN))
                    {
                        break;
                    }

                    receiveAudioPackets(packets, frameTsUs);

                    {
                        std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
                        m_audioAccumulator.erase(m_audioAccumulator.begin(), m_audioAccumulator.begin() + totalSamplesNeeded);
                        if (m_audioConfig.sampleRate > 0 && m_audioConfig.channels > 0)
                        {
                            int64_t consumedSamplesPerCh = static_cast<int64_t>(totalSamplesNeeded) /
                                                           static_cast<int64_t>(m_audioConfig.channels);
                            m_audioAccumulatorStartCaptureTsUs += (consumedSamplesPerCh * 1000000LL) /
                                                                   static_cast<int64_t>(m_audioConfig.sampleRate);
                        }
                        if (m_audioAccumulator.empty()) m_audioAccumulatorTsValid = false;
                    }
                }
                else
                {
                    break;
                }
            }

            // Flush resampler: send null input to get remaining samples from internal buffer
            SwrContext *swrCtx = static_cast<SwrContext *>(m_swrContext);
            if (swrCtx && audioFrame)
            {
                int64_t delay = swr_get_delay(swrCtx, m_audioConfig.sampleRate);
                if (delay > 0)
                {
                    const uint8_t *nullInput[1] = {nullptr};
                    int flushedSamples = swr_convert(swrCtx, audioFrame->data, samplesPerFrame,
                                                      nullInput, 0);

                    if (flushedSamples > 0)
                    {
                        audioFrame->nb_samples = flushedSamples;
                        int64_t frameTsUs = m_audioAccumulatorStartCaptureTsUs;
                        int64_t calculatedPTS = calculateAudioPTS(frameTsUs, flushedSamples * m_audioConfig.channels);
                        audioFrame->pts = calculatedPTS;
                        m_totalAudioSamplesProcessed += (flushedSamples * m_audioConfig.channels);

                        avcodec_send_frame(audioCtx, audioFrame);
                        receiveAudioPackets(packets, frameTsUs);
                    }
                }
            }

            // Padding final do accumulator com zero pra completar último frame
            {
                std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
                if (!m_audioAccumulator.empty() && m_audioAccumulator.size() < static_cast<size_t>(totalSamplesNeeded))
                {
                    m_audioAccumulator.resize(totalSamplesNeeded, 0);
                    std::vector<int16_t> samplesToProcess = m_audioAccumulator;
                    int64_t frameTsUs = m_audioAccumulatorStartCaptureTsUs;

                    if (convertInt16ToFloatPlanar(samplesToProcess.data(), totalSamplesNeeded, audioFrame, samplesPerFrame))
                    {
                        int actualFrameSamples = static_cast<AVFrame *>(audioFrame)->nb_samples;
                        int64_t calculatedPTS = calculateAudioPTS(frameTsUs, actualFrameSamples * m_audioConfig.channels);
                        audioFrame->pts = calculatedPTS;
                        m_totalAudioSamplesProcessed += (actualFrameSamples * m_audioConfig.channels);

                        avcodec_send_frame(audioCtx, audioFrame);
                        receiveAudioPackets(packets, frameTsUs);
                    }
                }
            }
        }
        
        // Now flush the codec
        avcodec_send_frame(audioCtx, nullptr);
        receiveAudioPackets(packets, 0);
    }
}

void MediaEncoder::cleanup()
{
    // SIMPLIFICADO: Apenas marcar como não inicializado e limpar estado
    // Não liberar recursos FFmpeg - deixar na memória para evitar crashes
    
    // Limpar apenas o acumulador de áudio (é seguro)
    {
        std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
        m_audioAccumulator.clear();
        m_audioAccumulatorStartCaptureTsUs = 0;
        m_audioAccumulatorTsValid = false;
    }

    m_initialized = false;
    m_videoFrameCount = 0;
    m_firstVideoTimestampSet = false;
    m_firstAudioTimestampSet = false;
    m_firstVideoTimestampUs = 0;
    m_firstAudioTimestampUs = 0;
    m_totalAudioSamplesProcessed = 0;
    m_audioFrameCount = 0;
    m_videoFrameCountForPTS = 0;
    m_desyncFrameCount.store(0, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(m_ptsMutex);
        m_lastVideoPTS = -1;
        m_lastVideoDTS = -1;
        m_lastVideoFramePTS = -1;
        m_lastAudioPTS = -1;
        m_lastAudioDTS = -1;
        m_lastAudioFramePTS = -1;
    }
    
    // NÃO liberar: m_swsContext, m_swrContext, m_videoFrame, m_audioFrame,
    // m_videoCodecContext, m_audioCodecContext
    // Deixar tudo na memória para evitar crashes durante cleanup
}
