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

#include "../utils/FFmpegCompat.h"

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

    // Acumular samples
    {
        std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
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

        // Conversion successful - now we can remove the samples from accumulator
        {
            std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
            // Remove samples that were sent to the resampler (totalSamplesNeeded)
            // The resampler may buffer some internally, but we've consumed them from input
            size_t sizeBefore = m_audioAccumulator.size();
            m_audioAccumulator.erase(m_audioAccumulator.begin(), m_audioAccumulator.begin() + totalSamplesNeeded);
            size_t sizeAfter = m_audioAccumulator.size();
            
            // Log accumulator status after processing
            if (frameLogCounter == 1 || frameLogCounter % 50 == 0)
            {
                LOG_INFO("MediaEncoder: After processing - accumulator: " + std::to_string(sizeBefore) + 
                         " -> " + std::to_string(sizeAfter) + 
                         " (removed " + std::to_string(totalSamplesNeeded) + ")");
            }
        }

        // Calcular timestamp real deste frame baseado em samples processados
        // Cada frame de áudio deve ter timestamp progressivo baseado em samples processados
        // timestamp = first_timestamp + (samples_processed_per_channel / sample_rate)
        int64_t frameTimestampUs = captureTimestampUs; // Fallback to chunk timestamp
        if (m_firstAudioTimestampSet && m_audioConfig.sampleRate > 0 && m_audioConfig.channels > 0)
        {
            // Calculate real timestamp: first_timestamp + (samples_processed_per_channel / sample_rate)
            int64_t samplesPerChannel = m_totalAudioSamplesProcessed / m_audioConfig.channels;
            int64_t durationUs = (samplesPerChannel * 1000000LL) / m_audioConfig.sampleRate;
            frameTimestampUs = m_firstAudioTimestampUs + durationUs;
        }
        
        // Calcular PTS baseado em samples processados (precise, no jitter)
        // Use actual samples in frame (nb_samples), not totalSamplesNeeded
        // This ensures PTS matches the actual audio data in the frame
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
    // Store first timestamp for reference, but calculate PTS based on samples processed
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

    // Calculate PTS based on samples processed and sample rate (precise, no jitter)
    // PTS = (samples_processed_per_channel / sample_rate) in time_base units
    // For time_base = {1, sampleRate}, PTS = samples_per_channel
    AVRational timeBase = codecCtx->time_base;
    int64_t calculatedPTS = 0;
    
    if (m_audioConfig.sampleRate > 0 && m_audioConfig.channels > 0)
    {
        // Calculate: (samples_processed / channels) * time_base.den / (sample_rate * time_base.num)
        // samples_processed is total samples (all channels) processed so far (BEFORE this frame)
        // This ensures each frame has a progressively increasing PTS
        int64_t samplesPerChannel = m_totalAudioSamplesProcessed / m_audioConfig.channels;
        // PTS = samples_per_channel / sample_rate in time_base units
        // Since time_base = {1, sampleRate}, this simplifies to: samples_per_channel
        // But we calculate it properly to handle any time_base
        calculatedPTS = (samplesPerChannel * timeBase.den) / (m_audioConfig.sampleRate * timeBase.num);
    }
    else
    {
        // Fallback: use timestamp-based calculation if sample rate unknown
        int64_t relativeTimeUs = captureTimestampUs - m_firstAudioTimestampUs;
        double relativeTimeSeconds = static_cast<double>(relativeTimeUs) / 1000000.0;
        calculatedPTS = static_cast<int64_t>(relativeTimeSeconds * timeBase.den / timeBase.num);
    }

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
        FFmpegCompat::setKeyFrame(videoFrame, true);
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
                {
                    std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
                    if (static_cast<int>(m_audioAccumulator.size()) < totalSamplesNeeded)
                    {
                        break; // Not enough samples for a complete frame
                    }
                    samplesToProcess.assign(m_audioAccumulator.begin(), m_audioAccumulator.begin() + totalSamplesNeeded);
                }
                
                // Convert and encode remaining samples
                if (convertInt16ToFloatPlanar(samplesToProcess.data(), totalSamplesNeeded, audioFrame, samplesPerFrame))
                {
                    int actualFrameSamples = static_cast<AVFrame *>(audioFrame)->nb_samples;
                    
                    // Calculate PTS for this frame
                    int64_t calculatedPTS = calculateAudioPTS(0, actualFrameSamples * m_audioConfig.channels);
                    audioFrame->pts = calculatedPTS;
                    m_totalAudioSamplesProcessed += (actualFrameSamples * m_audioConfig.channels);
                    
                    // Send to codec
                    int ret = avcodec_send_frame(audioCtx, audioFrame);
                    if (ret < 0 && ret != AVERROR(EAGAIN))
                    {
                        break;
                    }
                    
                    // Receive packets
                    receiveAudioPackets(packets, 0);
                    
                    // Remove processed samples
                    {
                        std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
                        m_audioAccumulator.erase(m_audioAccumulator.begin(), m_audioAccumulator.begin() + totalSamplesNeeded);
                    }
                }
                else
                {
                    break; // Conversion failed
                }
            }
            
            // Flush resampler: send null input to get remaining samples from internal buffer
            // This ensures all audio is processed, not lost
            SwrContext *swrCtx = static_cast<SwrContext *>(m_swrContext);
            if (swrCtx && audioFrame)
            {
                // Check if resampler has delayed samples
                int64_t delay = swr_get_delay(swrCtx, m_audioConfig.sampleRate);
                if (delay > 0)
                {
                    // Flush resampler by sending null input
                    const uint8_t *nullInput[1] = {nullptr};
                    int flushedSamples = swr_convert(swrCtx, audioFrame->data, samplesPerFrame,
                                                      nullInput, 0);
                    
                    if (flushedSamples > 0)
                    {
                        audioFrame->nb_samples = flushedSamples;
                        int64_t calculatedPTS = calculateAudioPTS(0, flushedSamples * m_audioConfig.channels);
                        audioFrame->pts = calculatedPTS;
                        m_totalAudioSamplesProcessed += (flushedSamples * m_audioConfig.channels);
                        
                        avcodec_send_frame(audioCtx, audioFrame);
                        receiveAudioPackets(packets, 0);
                    }
                }
            }
            
            // Process any remaining samples in accumulator (pad with zeros if needed)
            {
                std::lock_guard<std::mutex> lock(m_audioAccumulatorMutex);
                if (!m_audioAccumulator.empty() && m_audioAccumulator.size() < static_cast<size_t>(totalSamplesNeeded))
                {
                    // Pad with zeros to complete a frame
                    m_audioAccumulator.resize(totalSamplesNeeded, 0);
                    std::vector<int16_t> samplesToProcess = m_audioAccumulator;
                    
                    if (convertInt16ToFloatPlanar(samplesToProcess.data(), totalSamplesNeeded, audioFrame, samplesPerFrame))
                    {
                        int actualFrameSamples = static_cast<AVFrame *>(audioFrame)->nb_samples;
                        int64_t calculatedPTS = calculateAudioPTS(0, actualFrameSamples * m_audioConfig.channels);
                        audioFrame->pts = calculatedPTS;
                        m_totalAudioSamplesProcessed += (actualFrameSamples * m_audioConfig.channels);
                        
                        avcodec_send_frame(audioCtx, audioFrame);
                        receiveAudioPackets(packets, 0);
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
    m_totalAudioSamplesProcessed = 0;
    m_audioFrameCount = 0;
    m_videoFrameCountForPTS = 0;

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
