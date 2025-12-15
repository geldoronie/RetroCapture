#include "MediaEncoder.h"
#include "../utils/Logger.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

MediaEncoder::MediaEncoder()
{
}

MediaEncoder::~MediaEncoder()
{
    cleanup();
}

bool MediaEncoder::initialize(const VideoConfig &videoConfig, const AudioConfig &audioConfig)
{
    if (m_initialized)
    {
        cleanup();
    }

    m_videoConfig = videoConfig;
    m_audioConfig = audioConfig;

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
    codecCtx->time_base = {1, static_cast<int>(m_videoConfig.fps)};
    codecCtx->framerate = {static_cast<int>(m_videoConfig.fps), 1};
    codecCtx->gop_size = static_cast<int>(m_videoConfig.fps * 2);
    codecCtx->max_b_frames = 0;
    codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    codecCtx->bit_rate = m_videoConfig.bitrate;
    codecCtx->thread_count = 0;
    codecCtx->thread_type = FF_THREAD_SLICE;

    if (codec->id == AV_CODEC_ID_HEVC || codec->id == AV_CODEC_ID_VP8 || codec->id == AV_CODEC_ID_VP9)
    {
        codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Configurar opções específicas do codec
    AVDictionary *opts = nullptr;
    if (codec->id == AV_CODEC_ID_H264)
    {
        av_dict_set(&opts, "preset", m_videoConfig.preset.c_str(), 0);
        av_dict_set(&opts, "tune", "zerolatency", 0);
        av_dict_set(&opts, "profile", m_videoConfig.profile.c_str(), 0);
        int keyint = static_cast<int>(m_videoConfig.fps * 2);
        av_dict_set_int(&opts, "keyint_min", keyint, 0);
        av_dict_set_int(&opts, "keyint", keyint, 0);
        av_dict_set_int(&opts, "rc-lookahead", 0, 0);
        av_dict_set_int(&opts, "vbv-bufsize", m_videoConfig.bitrate / 10, 0);
        av_dict_set_int(&opts, "scenecut", 0, 0);
        av_dict_set_int(&opts, "repeat-headers", 1, 0);
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
        av_dict_set_int(&opts, "vbv-bufsize", m_videoConfig.bitrate / 10, 0);
        av_dict_set_int(&opts, "scenecut", 0, 0);
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
    #if LIBAVCODEC_VERSION_MAJOR >= 59
    av_channel_layout_default(&codecCtx->ch_layout, m_audioConfig.channels);
    #else
    codecCtx->channels = m_audioConfig.channels;
    codecCtx->channel_layout = av_get_default_channel_layout(m_audioConfig.channels);
    #endif
    codecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    codecCtx->bit_rate = m_audioConfig.bitrate;
    codecCtx->thread_count = 4;
    codecCtx->time_base = {1, static_cast<int>(m_audioConfig.sampleRate)};

    if (avcodec_open2(codecCtx, codec, nullptr) < 0)
    {
        LOG_ERROR("Failed to open audio codec");
        avcodec_free_context(&codecCtx);
        return false;
    }

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
    #if LIBAVCODEC_VERSION_MAJOR >= 59
    av_channel_layout_default(&audioFrame->ch_layout, m_audioConfig.channels);
    #else
    audioFrame->channels = m_audioConfig.channels;
    audioFrame->channel_layout = av_get_default_channel_layout(m_audioConfig.channels);
    #endif
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

    SwsContext *swsCtx = static_cast<SwsContext *>(m_swsContext);
    if (!swsCtx || m_swsSrcWidth != width || m_swsSrcHeight != height ||
        m_swsDstWidth != dstWidth || m_swsDstHeight != dstHeight)
    {
        if (swsCtx)
        {
            sws_freeContext(swsCtx);
        }

        swsCtx = sws_getContext(
            width, height, AV_PIX_FMT_RGB24,
            dstWidth, dstHeight, AV_PIX_FMT_YUV420P,
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
        return false;
    }

    if (av_frame_make_writable(frame) < 0)
    {
        return false;
    }

    const uint8_t *srcData[1] = {reinterpret_cast<const uint8_t *>(samples)};
    const int inputSamples = static_cast<int>(sampleCount / m_audioConfig.channels);

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

int64_t MediaEncoder::calculateVideoPTS(int64_t captureTimestampUs)
{
    if (!m_firstVideoTimestampSet)
    {
        m_firstVideoTimestampUs = captureTimestampUs;
        m_firstVideoTimestampSet = true;
    }

    int64_t relativeTimeUs = captureTimestampUs - m_firstVideoTimestampUs;
    double relativeTimeSeconds = static_cast<double>(relativeTimeUs) / 1000000.0;

    AVCodecContext *codecCtx = static_cast<AVCodecContext *>(m_videoCodecContext);
    if (!codecCtx)
    {
        return 0;
    }

    AVRational timeBase = codecCtx->time_base;
    int64_t calculatedPTS = static_cast<int64_t>(relativeTimeSeconds * timeBase.den / timeBase.num);

    {
        std::lock_guard<std::mutex> lock(m_ptsMutex);
        if (m_lastVideoFramePTS >= 0 && calculatedPTS <= m_lastVideoFramePTS)
        {
            calculatedPTS = m_lastVideoFramePTS + 1;
        }
        m_lastVideoFramePTS = calculatedPTS;
    }

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

    // Acumular samples
    {
        std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
        m_audioAccumulator.insert(m_audioAccumulator.end(), samples, samples + sampleCount);
    }

    const int samplesPerFrame = codecCtx->frame_size;
    if (samplesPerFrame <= 0)
    {
        return false;
    }

    const int totalSamplesNeeded = samplesPerFrame * m_audioConfig.channels;
    bool processedAny = false;

    // Processar frames completos enquanto houver samples suficientes
    while (true)
    {
        std::vector<int16_t> samplesToProcess;
        int64_t frameTimestampUs = captureTimestampUs;

        {
            std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
            if (static_cast<int>(m_audioAccumulator.size()) < totalSamplesNeeded)
            {
                break; // Não há samples suficientes
            }

            // Pegar samples para este frame
            samplesToProcess.assign(m_audioAccumulator.begin(), m_audioAccumulator.begin() + totalSamplesNeeded);
            m_audioAccumulator.erase(m_audioAccumulator.begin(), m_audioAccumulator.begin() + totalSamplesNeeded);
        }

        // Converter int16 para float planar
        if (!convertInt16ToFloatPlanar(samplesToProcess.data(), totalSamplesNeeded, audioFrame, samplesPerFrame))
        {
            LOG_ERROR("MediaEncoder: convertInt16ToFloatPlanar failed");
            break;
        }

        // Calcular PTS
        int64_t calculatedPTS = calculateAudioPTS(frameTimestampUs, totalSamplesNeeded);
        audioFrame->pts = calculatedPTS;

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
    if (!m_firstAudioTimestampSet)
    {
        m_firstAudioTimestampUs = captureTimestampUs;
        m_firstAudioTimestampSet = true;
    }

    int64_t relativeTimeUs = captureTimestampUs - m_firstAudioTimestampUs;
    double relativeTimeSeconds = static_cast<double>(relativeTimeUs) / 1000000.0;

    AVCodecContext *codecCtx = static_cast<AVCodecContext *>(m_audioCodecContext);
    if (!codecCtx)
    {
        return 0;
    }

    AVRational timeBase = codecCtx->time_base;
    int64_t calculatedPTS = static_cast<int64_t>(relativeTimeSeconds * timeBase.den / timeBase.num);

    {
        std::lock_guard<std::mutex> lock(m_ptsMutex);
        if (m_lastAudioFramePTS >= 0 && calculatedPTS <= m_lastAudioFramePTS)
        {
            calculatedPTS = m_lastAudioFramePTS + 1;
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
        if (pts != AV_NOPTS_VALUE_LOCAL)
        {
            if (m_lastVideoPTS >= 0 && pts <= m_lastVideoPTS)
            {
                pts = m_lastVideoPTS + 1;
            }
            m_lastVideoPTS = pts;
        }
        if (dts != AV_NOPTS_VALUE_LOCAL)
        {
            if (m_lastVideoDTS >= 0 && dts <= m_lastVideoDTS)
            {
                dts = m_lastVideoDTS + 1;
            }
            m_lastVideoDTS = dts;
        }
        if (pts != AV_NOPTS_VALUE_LOCAL && dts != AV_NOPTS_VALUE_LOCAL && dts > pts)
        {
            dts = pts;
            m_lastVideoDTS = dts;
        }
    }
    else
    {
        if (pts != AV_NOPTS_VALUE_LOCAL)
        {
            if (m_lastAudioPTS >= 0 && pts <= m_lastAudioPTS)
            {
                pts = m_lastAudioPTS + 1;
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
        #if LIBAVCODEC_VERSION_MAJOR >= 59
        videoFrame->flags |= AV_FRAME_FLAG_KEY;
        #else
        videoFrame->key_frame = 1;
        #endif
    }
    m_videoFrameCount++;

    int ret = avcodec_send_frame(codecCtx, videoFrame);
    if (ret < 0)
    {
        if (ret == AVERROR(EAGAIN))
        {
            receiveVideoPackets(packets, captureTimestampUs);
            ret = avcodec_send_frame(codecCtx, videoFrame);
        }
        if (ret < 0 && ret != AVERROR(EAGAIN))
        {
            LOG_ERROR("MediaEncoder: avcodec_send_frame failed");
            return false;
        }
    }

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
    if (audioCtx)
    {
        avcodec_send_frame(audioCtx, nullptr);
        receiveAudioPackets(packets, 0);
    }
}

void MediaEncoder::cleanup()
{
    if (m_swsContext)
    {
        sws_freeContext(static_cast<SwsContext *>(m_swsContext));
        m_swsContext = nullptr;
    }

    if (m_swrContext)
    {
        SwrContext *swrCtx = static_cast<SwrContext *>(m_swrContext);
        swr_free(&swrCtx);
        m_swrContext = nullptr;
    }

    if (m_videoFrame)
    {
        AVFrame *frame = static_cast<AVFrame *>(m_videoFrame);
        av_frame_free(&frame);
        m_videoFrame = nullptr;
    }

    if (m_audioFrame)
    {
        AVFrame *frame = static_cast<AVFrame *>(m_audioFrame);
        av_frame_free(&frame);
        m_audioFrame = nullptr;
    }

    if (m_videoCodecContext)
    {
        AVCodecContext *ctx = static_cast<AVCodecContext *>(m_videoCodecContext);
        avcodec_free_context(&ctx);
        m_videoCodecContext = nullptr;
    }

    if (m_audioCodecContext)
    {
        AVCodecContext *ctx = static_cast<AVCodecContext *>(m_audioCodecContext);
        avcodec_free_context(&ctx);
        m_audioCodecContext = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
        m_audioAccumulator.clear();
    }

    m_initialized = false;
    m_videoFrameCount = 0;
    m_firstVideoTimestampSet = false;
    m_firstAudioTimestampSet = false;
    m_firstVideoTimestampUs = 0;
    m_firstAudioTimestampUs = 0;

    {
        std::lock_guard<std::mutex> lock(m_ptsMutex);
        m_lastVideoPTS = -1;
        m_lastVideoDTS = -1;
        m_lastVideoFramePTS = -1;
        m_lastAudioPTS = -1;
        m_lastAudioDTS = -1;
        m_lastAudioFramePTS = -1;
    }
}
