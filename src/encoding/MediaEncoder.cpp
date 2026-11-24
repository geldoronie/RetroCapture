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
#include <libavutil/audio_fifo.h>
#include <libavutil/mathematics.h>
}

#include "../utils/FFmpegCompat.h"
#include <cstring>
#include <mutex>
#include <chrono>
#include <cmath>
#include <algorithm>

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

    // #129 — the FFmpeg 4.1 libx264/libx265 build shipped in our MXE Windows
    // toolchain emits empty (neutral-gray) frames whenever ABR rate control is
    // used: bisection proved that setting codecCtx->bit_rate produces a blank
    // stream at >=640x480 (both streaming and recording came out solid gray —
    // the long-standing Windows "gray screen / white noise" report), while
    // constant-quality CRF encodes the exact same input correctly. Linux/macOS
    // ship newer FFmpeg where ABR is fine, so this only applies to Windows.
    // Drive the software x264/x265 with pure constant-quality CRF — NO VBV.
    // The VBV/HRD rate-control model in this FFmpeg corrupts to solid-black
    // frames the moment the buffer underflows, which sustained real content
    // triggers a few seconds in (bisected: 920 underflows -> blank stream,
    // while pure CRF ran 2000 frames clean). ABR sets a bitrate AND a VBV, so
    // it's broken too; even CRF + an explicit vbv-maxrate cap reintroduces the
    // underflow. Pure CRF has no buffer state to break. The trade-off is an
    // unbounded peak bitrate, far preferable to a black stream; CRF still
    // self-limits to a sane average for typical capture content.
#ifdef _WIN32
    const bool useCRF = (codec->id == AV_CODEC_ID_H264 || codec->id == AV_CODEC_ID_HEVC);
#else
    const bool useCRF = false;
#endif
    if (!useCRF)
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

        if (useCRF)
        {
            // #129 — Windows constant-quality path (ABR + VBV are broken here,
            // see above). NO vbv-maxrate/vbv-bufsize: any VBV reintroduces the
            // underflow-to-black bug. repeat-headers=1 inlines SPS/PPS before
            // every IDR for mid-stream MPEG-TS joins. x264-params is the channel
            // that actually reaches libx264 (bare AVOption keys are dropped by
            // this FFmpeg).
            av_dict_set_int(&opts, "crf", 20, 0);
            av_dict_set(&opts, "x264-params", "repeat-headers=1", 0);
        }
        else if (m_forStreaming)
        {
            // HTTP-TS streaming: tight vbv caps bitrate variation.
            av_dict_set_int(&opts, "vbv-bufsize", m_videoConfig.bitrate / 10, 0);
            av_dict_set_int(&opts, "repeat-headers", 1, 0);
        }
        else
        {
            // Gravação em arquivo: vbv largo pra rate-control flexível.
            av_dict_set_int(&opts, "vbv-bufsize", m_videoConfig.bitrate * 2, 0);
            // Inline SPS/PPS in front of every IDR so thumbnailers in DaVinci /
            // kdenlive / Premiere that seek to a random byte offset can decode
            // the next keyframe immediately (#59).
            av_dict_set_int(&opts, "repeat-headers", 1, 0);
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

        if (useCRF)
        {
            // #129 — Windows constant-quality path, no VBV (mirrors the H.264
            // path: any VBV in this FFmpeg corrupts to black on underflow).
            av_dict_set_int(&opts, "crf", 22, 0);
            av_dict_set(&opts, "x265-params",
                        m_forStreaming ? "repeat-headers=1:annexb=1" : "repeat-headers=1", 0);
        }
        else if (m_forStreaming)
        {
            av_dict_set_int(&opts, "vbv-bufsize", m_videoConfig.bitrate / 10, 0);
            // Streaming uses Annex B byte-stream framing inside the
            // MPEG-TS PES payload; repeat-headers=1 emits VPS/SPS/PPS
            // ahead of every IDR so mid-stream MPEG-TS clients can
            // decode immediately. Without this the client sees
            // 'Invalid NAL unit 35', 'Truncating likely oversized VPS',
            // 'PCM bit depth out of range'.
            av_dict_set(&opts, "x265-params", "repeat-headers=1:annexb=1", 0);
        }
        else
        {
            av_dict_set_int(&opts, "vbv-bufsize", m_videoConfig.bitrate * 2, 0);
            // File recording (#59): repeat-headers=1 also benefits
            // mid-file scrubbing in DaVinci / kdenlive / Premiere
            // thumbnail strips, which seek to arbitrary byte offsets
            // and decode forward — they get a clean VPS/SPS/PPS at
            // the very next IDR instead of relying on hvcC extradata,
            // which some scrubbers skip when reading sequentially.
            // No annexb=1: MP4 carries HEVC in length-prefix format.
            av_dict_set(&opts, "x265-params", "repeat-headers=1", 0);
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

    // #199 — the resampler converts the capture's S16 interleaved input to
    // the encoder's FLTP at the encoder rate AND applies the drift
    // compensation that keeps audio on the capture clock. Its input format
    // follows the actual capture (rebuilt if a chunk arrives in another
    // format), so a 48 kHz device feeding a 44.1 kHz encoder is resampled
    // instead of played at the wrong speed.
    if (!createAudioResampler(m_audioConfig.sampleRate, m_audioConfig.channels))
    {
        avcodec_free_context(&codecCtx);
        m_audioCodecContext = nullptr;
        return false;
    }
    SwrContext *swrCtx = static_cast<SwrContext *>(m_swrContext);

    m_audioFifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, static_cast<int>(m_audioConfig.channels),
                                      static_cast<int>(m_audioConfig.sampleRate));
    if (!m_audioFifo)
    {
        LOG_ERROR("Failed to allocate audio FIFO");
        swr_free(&swrCtx);
        m_swrContext = nullptr;
        avcodec_free_context(&codecCtx);
        m_audioCodecContext = nullptr;
        return false;
    }

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

    // libswscale's AVX2 RGB fastpath (macOS Homebrew FFmpeg) reads a
    // few SIMD chunks (up to ~32 bytes) PAST the last source row via
    // vinserti128/vmovdqa. When the caller's RGB buffer is allocated
    // exactly width*height*3 and happens to end on a page boundary,
    // that over-read hits an unmapped page and segfaults (observed on
    // a macOS host with a 1280x720 buffer = exactly 2700K, crashing
    // 1 byte past the MALLOC region). An earlier fix only clamped odd
    // heights, which doesn't cover this: the over-read happens off the
    // last row regardless of parity.
    //
    // Robust fix: copy the source into a scratch buffer with trailing
    // padding so the fastpath's over-read always lands in allocated
    // memory. The pad bytes are read into discarded (off-image) pixels.
    if (height == 0 || width == 0 || !rgbData)
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

    const int srcStride = static_cast<int>(width * 3);
    const size_t srcBytes = static_cast<size_t>(srcStride) * height;

    // Copy into the padded scratch so libswscale's AVX2 fastpath can
    // safely over-read past the last row (see the note above). 64 is
    // FFmpeg's AV_INPUT_BUFFER_PADDING_SIZE; the fastpath reads at most
    // a couple of SIMD registers past the end, well within that.
    constexpr size_t kSwsTailPad = 64;
    if (m_swsSrcPadded.size() < srcBytes + kSwsTailPad)
    {
        m_swsSrcPadded.resize(srcBytes + kSwsTailPad);
    }
    std::memcpy(m_swsSrcPadded.data(), rgbData, srcBytes);

    const uint8_t *srcData[1] = {m_swsSrcPadded.data()};
    int srcLinesize[1] = {srcStride};

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

bool MediaEncoder::createAudioResampler(uint32_t inRate, uint32_t inChannels)
{
    if (inRate == 0 || inChannels == 0)
    {
        return false;
    }
    SwrContext *old = static_cast<SwrContext *>(m_swrContext);
    if (old)
    {
        swr_free(&old);
        m_swrContext = nullptr;
    }

    SwrContext *swrCtx = swr_alloc();
    if (!swrCtx)
    {
        LOG_ERROR("Failed to allocate SWR context");
        return false;
    }
#if LIBAVCODEC_VERSION_MAJOR >= 59
    AVChannelLayout inChLayout, outChLayout;
    av_channel_layout_default(&inChLayout, static_cast<int>(inChannels));
    av_channel_layout_default(&outChLayout, static_cast<int>(m_audioConfig.channels));
    av_opt_set_chlayout(swrCtx, "in_chlayout", &inChLayout, 0);
    av_opt_set_chlayout(swrCtx, "out_chlayout", &outChLayout, 0);
    av_channel_layout_uninit(&inChLayout);
    av_channel_layout_uninit(&outChLayout);
#else
    av_opt_set_int(swrCtx, "in_channel_layout", av_get_default_channel_layout(static_cast<int>(inChannels)), 0);
    av_opt_set_int(swrCtx, "out_channel_layout", av_get_default_channel_layout(static_cast<int>(m_audioConfig.channels)), 0);
#endif
    av_opt_set_int(swrCtx, "in_sample_rate", static_cast<int>(inRate), 0);
    av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    av_opt_set_int(swrCtx, "out_sample_rate", static_cast<int>(m_audioConfig.sampleRate), 0);
    av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_FLTP, 0);
    // Always run the real resampler (even at equal rates) so
    // swr_set_compensation() has something to act on without re-initialising.
    av_opt_set_int(swrCtx, "swr_flags", SWR_FLAG_RESAMPLE, 0);

    if (swr_init(swrCtx) < 0)
    {
        LOG_ERROR("Failed to initialize SWR context");
        swr_free(&swrCtx);
        return false;
    }
    m_swrContext = swrCtx;
    m_swrInRate = inRate;
    m_swrInChannels = inChannels;
    if (inRate != m_audioConfig.sampleRate || inChannels != m_audioConfig.channels)
    {
        LOG_INFO("MediaEncoder: audio input " + std::to_string(inRate) + " Hz x " +
                 std::to_string(inChannels) + " ch resampled to " +
                 std::to_string(m_audioConfig.sampleRate) + " Hz x " +
                 std::to_string(m_audioConfig.channels) + " ch");
    }
    return true;
}

int64_t MediaEncoder::calculateVideoPTS(int64_t captureTimestampUs)
{
    // #199 — capture timestamps (#197/#198) put audio and video on one
    // clock. The first VIDEO frame latches the shared epoch: audio
    // captured before it is trimmed, so the output starts with both
    // streams at t=0 instead of leading audio (the /stream encoder used to
    // receive audio for as long as it waited for its first client).
    if (!m_epochSet)
    {
        m_epochUs = captureTimestampUs;
        m_epochSet = true;
    }

    AVCodecContext *codecCtx = static_cast<AVCodecContext *>(m_videoCodecContext);
    if (!codecCtx)
    {
        return -1;
    }

    const int64_t relativeTimeUs = captureTimestampUs - m_epochUs;
    if (relativeTimeUs < 0)
    {
        return -1; // captured before the epoch
    }
    m_videoFrameCountForPTS++;
    return av_rescale_q(relativeTimeUs, AVRational{1, 1000000}, codecCtx->time_base);
}

bool MediaEncoder::encodeAudio(const int16_t *samples, size_t sampleCount,
                               int64_t captureTimestampUs, uint32_t inRate, uint32_t inChannels,
                               std::vector<EncodedPacket> &packets)
{
    if (!samples || !m_initialized || sampleCount == 0)
    {
        return false;
    }

    AVCodecContext *codecCtx = static_cast<AVCodecContext *>(m_audioCodecContext);
    AVAudioFifo *fifo = static_cast<AVAudioFifo *>(m_audioFifo);
    if (!codecCtx || !fifo)
    {
        return false;
    }
    if (inRate == 0) inRate = m_audioConfig.sampleRate;
    if (inChannels == 0) inChannels = m_audioConfig.channels;
    if (inRate != m_swrInRate || inChannels != m_swrInChannels)
    {
        // Capture format changed (device switch): rebuild the resampler and
        // re-anchor on the next chunk.
        if (!createAudioResampler(inRate, inChannels))
        {
            return false;
        }
        m_audioStarted = false;
    }
    SwrContext *swrCtx = static_cast<SwrContext *>(m_swrContext);

    // No video yet → no epoch → nothing to align to. Dropping here is what
    // keeps a not-yet-watched /stream from accumulating leading audio.
    if (!m_epochSet)
    {
        return false;
    }

    const double outRate = static_cast<double>(m_audioConfig.sampleRate);
    const double inToOut = outRate / static_cast<double>(inRate);
    const int64_t inFramesTotal = static_cast<int64_t>(sampleCount / inChannels);
    const int16_t *in = samples;
    int64_t inFrames = inFramesTotal;

    // Where this chunk's first sample belongs on the output timeline
    // (output samples since the epoch).
    double targetPos = static_cast<double>(captureTimestampUs - m_epochUs) * outRate / 1e6;

    if (!m_audioStarted)
    {
        if (targetPos + static_cast<double>(inFrames) * inToOut <= 0.0)
        {
            return false; // entirely before the epoch
        }
        if (targetPos < 0.0)
        {
            const int64_t skip = std::min<int64_t>(inFrames,
                                                   static_cast<int64_t>(std::ceil(-targetPos / inToOut)));
            in += skip * inChannels;
            inFrames -= skip;
            targetPos += static_cast<double>(skip) * inToOut;
        }
        m_audioOutPos = static_cast<int64_t>(std::llround(targetPos));
        m_audioDriftEma = 0.0;
        m_audioStarted = true;
        swr_set_compensation(swrCtx, 0, 0);
    }
    else
    {
        // Timeline position the next produced sample will get: what was
        // already handed to the encoder + what waits in the FIFO + what the
        // resampler still holds.
        const double queued = static_cast<double>(av_audio_fifo_size(fifo)) +
                              static_cast<double>(swr_get_delay(swrCtx, m_audioConfig.sampleRate));
        const double currentPos = static_cast<double>(m_audioOutPos) + queued;
        // > 0: our samples would play later than they were captured.
        const double err = currentPos - targetPos;
        const double hardLimit = 0.2 * outRate; // 200 ms

        if (err < -hardLimit)
        {
            // A real gap (capture stall, dropped chunks, device hiccup):
            // fill it with silence so the following audio lands on time.
            const int64_t gap = static_cast<int64_t>(std::llround(-err));
            writeSilenceToFifo(gap);
            m_audioDriftEma = 0.0;
            swr_set_compensation(swrCtx, 0, 0);
            const uint64_t n = ++m_audioGapsFilled;
            if (n <= 5 || n % 50 == 0)
                LOG_WARN("MediaEncoder: audio gap of " + std::to_string(gap * 1000 / m_audioConfig.sampleRate) +
                         " ms filled with silence (total " + std::to_string(n) + ")");
        }
        else if (err > hardLimit)
        {
            // Audio is far ahead of its capture time (timestamps jumped
            // back, or a burst we can't place): drop the excess input.
            const int64_t drop = std::min<int64_t>(inFrames,
                                                   static_cast<int64_t>(std::llround(err / inToOut)));
            in += drop * inChannels;
            inFrames -= drop;
            m_audioDriftEma = 0.0;
            swr_set_compensation(swrCtx, 0, 0);
            const uint64_t n = ++m_audioOverlapsDropped;
            if (n <= 5 || n % 50 == 0)
                LOG_WARN("MediaEncoder: dropped " + std::to_string(drop * 1000 / static_cast<int64_t>(inRate)) +
                         " ms of audio ahead of its capture time (total " + std::to_string(n) + ")");
            if (inFrames <= 0)
            {
                return false;
            }
        }
        else
        {
            // Soft lock: the capture timestamps jitter by a few ms, the
            // device clock drifts by ppm. Smooth the error, then steer the
            // resampler rate to bleed it off over ~5 s, capped at 0.5 %
            // (inaudible). Steady-state error under a 1000 ppm skewed
            // device is ~5 ms.
            m_audioDriftEma += 0.05 * (err - m_audioDriftEma);
            double ratio = -m_audioDriftEma / (outRate * 5.0);
            ratio = std::max(-0.005, std::min(0.005, ratio));
            const int distance = static_cast<int>(m_audioConfig.sampleRate);
            const int delta = static_cast<int>(std::lround(ratio * distance));
            swr_set_compensation(swrCtx, delta, distance);
        }
    }

    if (inFrames > 0)
    {
        // Resample into the FIFO.
        const int maxOut = static_cast<int>(swr_get_out_samples(swrCtx, static_cast<int>(inFrames))) + 64;
        if (maxOut <= 0)
        {
            return false;
        }
        const int outChannels = static_cast<int>(m_audioConfig.channels);
        m_audioPlanes.resize(static_cast<size_t>(outChannels));
        std::vector<uint8_t *> planePtrs(static_cast<size_t>(outChannels));
        for (int c = 0; c < outChannels; ++c)
        {
            m_audioPlanes[c].resize(static_cast<size_t>(maxOut));
            planePtrs[c] = reinterpret_cast<uint8_t *>(m_audioPlanes[c].data());
        }
        const uint8_t *inPtr[1] = {reinterpret_cast<const uint8_t *>(in)};
        const int got = swr_convert(swrCtx, planePtrs.data(), maxOut, inPtr, static_cast<int>(inFrames));
        if (got < 0)
        {
            LOG_ERROR("MediaEncoder: swr_convert failed: " + std::to_string(got));
            return false;
        }
        if (got > 0)
        {
            av_audio_fifo_write(fifo, reinterpret_cast<void **>(planePtrs.data()), got);
        }
    }

    return drainAudioFifo(packets, captureTimestampUs, false);
}

void MediaEncoder::writeSilenceToFifo(int64_t frames)
{
    AVAudioFifo *fifo = static_cast<AVAudioFifo *>(m_audioFifo);
    if (!fifo || frames <= 0)
    {
        return;
    }
    const int outChannels = static_cast<int>(m_audioConfig.channels);
    const int chunk = 4096;
    std::vector<float> zeros(static_cast<size_t>(chunk), 0.0f);
    std::vector<void *> ptrs(static_cast<size_t>(outChannels), zeros.data());
    while (frames > 0)
    {
        const int n = static_cast<int>(std::min<int64_t>(frames, chunk));
        av_audio_fifo_write(fifo, ptrs.data(), n);
        frames -= n;
    }
}

bool MediaEncoder::drainAudioFifo(std::vector<EncodedPacket> &packets, int64_t captureTimestampUs, bool final)
{
    AVCodecContext *codecCtx = static_cast<AVCodecContext *>(m_audioCodecContext);
    AVAudioFifo *fifo = static_cast<AVAudioFifo *>(m_audioFifo);
    AVFrame *audioFrame = static_cast<AVFrame *>(m_audioFrame);
    if (!codecCtx || !fifo || !audioFrame)
    {
        return false;
    }
    const int frameSize = codecCtx->frame_size > 0 ? codecCtx->frame_size : 1024;
    if (final)
    {
        // Pad the tail to a whole codec frame so no audio is lost.
        const int rem = av_audio_fifo_size(fifo) % frameSize;
        if (rem > 0)
        {
            writeSilenceToFifo(frameSize - rem);
        }
    }

    bool producedAny = false;
    while (av_audio_fifo_size(fifo) >= frameSize)
    {
        if (av_frame_make_writable(audioFrame) < 0)
        {
            LOG_ERROR("MediaEncoder: audio frame not writable");
            return producedAny;
        }
        audioFrame->nb_samples = frameSize;
        if (av_audio_fifo_read(fifo, reinterpret_cast<void **>(audioFrame->data), frameSize) < frameSize)
        {
            break;
        }

        // Sample-count PTS on the output timeline (smooth), which the drift
        // controller above keeps aligned with the capture clock.
        int64_t pts = av_rescale_q(m_audioOutPos, AVRational{1, static_cast<int>(m_audioConfig.sampleRate)},
                                   codecCtx->time_base);
        m_audioOutPos += frameSize;
        {
            std::lock_guard<std::mutex> lock(m_ptsMutex);
            if (m_lastAudioFramePTS >= 0 && pts <= m_lastAudioFramePTS)
            {
                pts = m_lastAudioFramePTS + 1;
                uint64_t total = m_desyncFrameCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (total == 1 || (total % 60) == 0)
                {
                    LOG_WARN("MediaEncoder: PTS retrocession on audio frame (total: " +
                             std::to_string(total) + ")");
                }
            }
            m_lastAudioFramePTS = pts;
        }
        audioFrame->pts = pts;
        m_audioFrameCount++;

        int ret = avcodec_send_frame(codecCtx, audioFrame);
        if (ret == AVERROR(EAGAIN))
        {
            receiveAudioPackets(packets, captureTimestampUs);
            ret = avcodec_send_frame(codecCtx, audioFrame);
        }
        if (ret < 0 && ret != AVERROR(EAGAIN))
        {
            LOG_ERROR("MediaEncoder: avcodec_send_frame (audio) failed: " + std::to_string(ret));
            break;
        }
        if (receiveAudioPackets(packets, captureTimestampUs))
        {
            producedAny = true;
        }
    }
    return producedAny;
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

    // #123 — time each encode stage so the streaming telemetry can show
    // whether the CPU swscale convert, the HW upload, or the codec itself
    // is the bottleneck when the synchronizer overflows at 60 fps.
    using stage_clock = std::chrono::steady_clock;
    auto stageT0 = stage_clock::now();

    if (!convertRGBToYUV(rgbData, width, height, videoFrame))
    {
        LOG_ERROR("MediaEncoder: convertRGBToYUV failed");
        return false;
    }
    auto stageAfterConvert = stage_clock::now();

    int64_t calculatedPTS = calculateVideoPTS(captureTimestampUs);
    // #199 — never encode a frame captured before the epoch or not after
    // the previous one: out-of-order frames are what showed up as
    // back-and-forth stutter in recordings.
    if (calculatedPTS < 0 || calculatedPTS <= m_lastVideoFramePTS)
    {
        const uint64_t n = ++m_videoFramesOutOfOrder;
        if (n <= 5 || n % 100 == 0)
            LOG_WARN("MediaEncoder: dropped out-of-order video frame (total " + std::to_string(n) + ")");
        return false;
    }
    m_lastVideoFramePTS = calculatedPTS;
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
    auto stageBeforeUpload = stage_clock::now();
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
    auto stageAfterUpload = stage_clock::now();

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

    bool recvOk = receiveVideoPackets(packets, captureTimestampUs);
    auto stageEnd = stage_clock::now();

    // #123 — accumulate per-stage microseconds for the telemetry.
    using us = std::chrono::microseconds;
    m_convertUs.fetch_add(std::chrono::duration_cast<us>(stageAfterConvert - stageT0).count(),
                          std::memory_order_relaxed);
    m_uploadUs.fetch_add(std::chrono::duration_cast<us>(stageAfterUpload - stageBeforeUpload).count(),
                         std::memory_order_relaxed);
    m_encodeUs.fetch_add(std::chrono::duration_cast<us>(stageEnd - stageAfterUpload).count(),
                         std::memory_order_relaxed);
    m_stageFrames.fetch_add(1, std::memory_order_relaxed);

    return recvOk;
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
        // #199 — push what the resampler still holds and the FIFO tail
        // (padded to a whole frame) through the encoder.
        SwrContext *swrCtx = static_cast<SwrContext *>(m_swrContext);
        AVAudioFifo *fifo = static_cast<AVAudioFifo *>(m_audioFifo);
        if (m_audioStarted && swrCtx && fifo)
        {
            const int outChannels = static_cast<int>(m_audioConfig.channels);
            const int maxOut = static_cast<int>(swr_get_out_samples(swrCtx, 0)) + 64;
            if (maxOut > 0)
            {
                m_audioPlanes.resize(static_cast<size_t>(outChannels));
                std::vector<uint8_t *> planePtrs(static_cast<size_t>(outChannels));
                for (int c = 0; c < outChannels; ++c)
                {
                    m_audioPlanes[c].resize(static_cast<size_t>(maxOut));
                    planePtrs[c] = reinterpret_cast<uint8_t *>(m_audioPlanes[c].data());
                }
                const int got = swr_convert(swrCtx, planePtrs.data(), maxOut, nullptr, 0);
                if (got > 0)
                {
                    av_audio_fifo_write(fifo, reinterpret_cast<void **>(planePtrs.data()), got);
                }
            }
            drainAudioFifo(packets, 0, true);
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
    
    // Reset the audio timeline (#199). The FIFO is emptied, not freed —
    // same "keep FFmpeg objects alive" policy as below.
    if (m_audioFifo)
    {
        av_audio_fifo_reset(static_cast<AVAudioFifo *>(m_audioFifo));
    }
    m_audioStarted = false;
    m_audioOutPos = 0;
    m_audioDriftEma = 0.0;
    m_audioGapsFilled = 0;
    m_audioOverlapsDropped = 0;
    m_videoFramesOutOfOrder = 0;

    m_initialized = false;
    m_videoFrameCount = 0;
    m_epochSet = false;
    m_epochUs = 0;
    m_audioFrameCount = 0;
    m_videoFrameCountForPTS = 0;
    m_desyncFrameCount.store(0, std::memory_order_relaxed);
    m_convertUs.store(0, std::memory_order_relaxed);
    m_uploadUs.store(0, std::memory_order_relaxed);
    m_encodeUs.store(0, std::memory_order_relaxed);
    m_stageFrames.store(0, std::memory_order_relaxed);

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
