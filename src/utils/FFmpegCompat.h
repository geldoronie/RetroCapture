#pragma once

/**
 * FFmpeg Compatibility Layer
 *
 * Provides compatibility macros and functions for different FFmpeg versions
 * across Linux distributions (Ubuntu, Manjaro, etc.)
 *
 * Version differences:
 * - FFmpeg 5.8 (libavcodec 58): Uses channels/channel_layout
 * - FFmpeg 6.0 (libavcodec 60): Uses ch_layout (new API)
 * - FFmpeg 6.2+ (libavcodec 62+): Uses ch_layout (new API)
 *
 * IMPORTANT: Include this header AFTER including FFmpeg headers
 */

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

// Detect FFmpeg version
#ifndef LIBAVCODEC_VERSION_MAJOR
#error "libavcodec version not detected. Make sure libavcodec headers are included before FFmpegCompat.h"
#endif

// Compatibility macros for channel layout API
// FFmpeg 5.9+ (libavcodec 59+) uses new ch_layout API
#if LIBAVCODEC_VERSION_MAJOR >= 59
#define FFMPEG_USE_NEW_CHANNEL_LAYOUT 1
#else
#define FFMPEG_USE_NEW_CHANNEL_LAYOUT 0
#endif

// Compatibility macros for AVIO write callback
// FFmpeg 6.1+ (libavformat 61+) uses const uint8_t* for write_packet callback
// FFmpeg 6.0 (libavformat 60) still uses uint8_t* (non-const)
#if LIBAVFORMAT_VERSION_MAJOR >= 61
#define FFMPEG_USE_CONST_WRITE_CALLBACK 1
#else
#define FFMPEG_USE_CONST_WRITE_CALLBACK 0
#endif

// Helper functions for channel layout setup
namespace FFmpegCompat
{
    /**
     * Set channel layout for AVCodecContext
     * Compatible with FFmpeg 5.8 (58) through 6.2+ (62+)
     */
    inline void setChannelLayout(AVCodecContext *ctx, int channels)
    {
#if FFMPEG_USE_NEW_CHANNEL_LAYOUT
        av_channel_layout_default(&ctx->ch_layout, channels);
#else
        ctx->channels = channels;
        ctx->channel_layout = av_get_default_channel_layout(channels);
#endif
    }

    /**
     * Set channel layout for AVFrame
     * Compatible with FFmpeg 5.8 (58) through 6.2+ (62+)
     */
    inline void setFrameChannelLayout(AVFrame *frame, int channels)
    {
#if FFMPEG_USE_NEW_CHANNEL_LAYOUT
        av_channel_layout_default(&frame->ch_layout, channels);
#else
        frame->channels = channels;
        frame->channel_layout = av_get_default_channel_layout(channels);
#endif
    }

    /**
     * Uninitialize channel layout
     * Only needed for FFmpeg 5.9+ (59+)
     */
    inline void uninitChannelLayout(AVCodecContext *ctx)
    {
#if FFMPEG_USE_NEW_CHANNEL_LAYOUT
        av_channel_layout_uninit(&ctx->ch_layout);
#endif
    }

    /**
     * Uninitialize channel layout for AVFrame
     * Only needed for FFmpeg 5.9+ (59+)
     */
    inline void uninitFrameChannelLayout(AVFrame *frame)
    {
#if FFMPEG_USE_NEW_CHANNEL_LAYOUT
        av_channel_layout_uninit(&frame->ch_layout);
#endif
    }

    /**
     * Set channel layout for SwrContext (swresample)
     * Compatible with FFmpeg 5.8 (58) through 6.2+ (62+)
     */
    inline void setSwrChannelLayout(void *swrCtx, const char *optName, int channels)
    {
#if FFMPEG_USE_NEW_CHANNEL_LAYOUT
        AVChannelLayout chLayout;
        av_channel_layout_default(&chLayout, channels);
        av_opt_set_chlayout(swrCtx, optName, &chLayout, 0);
        av_channel_layout_uninit(&chLayout);
#else
        av_opt_set_int(swrCtx, optName, av_get_default_channel_layout(channels), 0);
#endif
    }

    /**
     * Set key frame flag for AVFrame
     * Compatible with FFmpeg 5.8 (58) through 6.2+ (62+)
     */
    inline void setKeyFrame(AVFrame *frame, bool isKeyFrame)
    {
#if FFMPEG_USE_NEW_CHANNEL_LAYOUT
        if (isKeyFrame)
            frame->flags |= AV_FRAME_FLAG_KEY;
        else
            frame->flags &= ~AV_FRAME_FLAG_KEY;
#else
        frame->key_frame = isKeyFrame ? 1 : 0;
#endif
    }
}
