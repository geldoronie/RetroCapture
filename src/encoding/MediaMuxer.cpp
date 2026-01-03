#include "MediaMuxer.h"
#include "../utils/Logger.h"
#include <cstring>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h> // Para av_rescale_q
}

// Callback wrapper para FFmpeg (precisa ser estático)
// Diferentes versões do FFmpeg têm assinaturas diferentes:
// - FFmpeg 6.1+ (libavformat 61+): const uint8_t*
// - FFmpeg 6.0- (libavformat 60-): uint8_t* (não const)
// Usamos const uint8_t* (mais seguro) e fazemos cast quando necessário
static int writeCallback(void *opaque, const uint8_t *buf, int buf_size)
{
    MediaMuxer *muxer = static_cast<MediaMuxer *>(opaque);
    if (!muxer)
    {
        return -1;
    }

    // Capturar header do formato (primeiros 64KB)
    muxer->captureFormatHeader(buf, buf_size);

    // Chamar callback customizado
    // Acessar m_writeCallback através de método público ou friend
    // Por enquanto, vamos usar um método público para acessar o callback
    return muxer->callWriteCallback(buf, buf_size);
}

// Wrapper para compatibilidade com versões do FFmpeg que esperam uint8_t* (não const)
// FFmpeg 6.0 (libavformat 60) ainda usa uint8_t* (não const)
static int writeCallbackNonConst(void *opaque, uint8_t *buf, int buf_size)
{
    return writeCallback(opaque, const_cast<const uint8_t*>(buf), buf_size);
}

// Incluir FFmpegCompat.h após definir callbacks para ter acesso às macros
#include "../utils/FFmpegCompat.h"

MediaMuxer::MediaMuxer()
{
}

MediaMuxer::~MediaMuxer()
{
    // cleanup();
}

bool MediaMuxer::initialize(const MediaEncoder::VideoConfig &videoConfig,
                            const MediaEncoder::AudioConfig &audioConfig,
                            void *videoCodecContext,
                            void *audioCodecContext,
                            WriteCallback writeCallback,
                            size_t avioBufferSize)
{
    if (m_initialized)
    {
        cleanup();
    }

    m_videoConfig = videoConfig;
    m_audioConfig = audioConfig;
    m_writeCallback = writeCallback;

    if (!videoCodecContext || !audioCodecContext)
    {
        LOG_ERROR("MediaMuxer: Codec contexts must be provided");
        return false;
    }

    // Armazenar codec contexts para conversão de PTS/DTS
    m_videoCodecContext = videoCodecContext;
    m_audioCodecContext = audioCodecContext;

    // Armazenar tamanho do buffer AVIO
    m_avioBufferSize = (avioBufferSize > 0) ? avioBufferSize : (256 * 1024); // 256KB padrão se 0

    if (!initializeStreams(videoCodecContext, audioCodecContext, m_avioBufferSize))
    {
        cleanup();
        return false;
    }

    m_initialized = true;
    return true;
}

bool MediaMuxer::initializeStreams(void *videoCodecContext, void *audioCodecContext, size_t avioBufferSize)
{
    AVCodecContext *videoCtx = static_cast<AVCodecContext *>(videoCodecContext);
    AVCodecContext *audioCtx = static_cast<AVCodecContext *>(audioCodecContext);

    if (!videoCtx || !audioCtx)
    {
        LOG_ERROR("MediaMuxer: Invalid codec contexts");
        return false;
    }

    // Criar contexto de formato
    AVFormatContext *formatCtx = avformat_alloc_context();
    if (!formatCtx)
    {
        LOG_ERROR("MediaMuxer: Failed to allocate format context");
        return false;
    }

    formatCtx->oformat = av_guess_format("mpegts", nullptr, nullptr);
    if (!formatCtx->oformat)
    {
        LOG_ERROR("MediaMuxer: Failed to guess muxer format");
        avformat_free_context(formatCtx);
        return false;
    }

    formatCtx->url = av_strdup("pipe:");
    if (!formatCtx->url)
    {
        LOG_ERROR("MediaMuxer: Failed to allocate muxer URL");
        avformat_free_context(formatCtx);
        return false;
    }

    // Configurar callback de escrita com tamanho configurável
    // Diferentes versões do FFmpeg esperam assinaturas diferentes:
    // - FFmpeg 6.1+ (libavformat 61+): const uint8_t*
    // - FFmpeg 6.0- (libavformat 60-): uint8_t* (não const)
    const size_t bufferSize = avioBufferSize; // Tamanho já validado em initialize()
    
    // Usar callback apropriado baseado na versão
    #if FFMPEG_USE_CONST_WRITE_CALLBACK
        // FFmpeg 6.1+: usar callback com const uint8_t*
        formatCtx->pb = avio_alloc_context(
            static_cast<unsigned char *>(av_malloc(bufferSize)), bufferSize,
            1, this, nullptr, writeCallback, nullptr);
    #else
        // FFmpeg 6.0-: usar wrapper com uint8_t* (não const)
        formatCtx->pb = avio_alloc_context(
            static_cast<unsigned char *>(av_malloc(bufferSize)), bufferSize,
            1, this, nullptr, writeCallbackNonConst, nullptr);
    #endif
    if (!formatCtx->pb)
    {
        LOG_ERROR("MediaMuxer: Failed to allocate AVIO context");
        av_free(const_cast<char *>(formatCtx->url));
        avformat_free_context(formatCtx);
        return false;
    }

    // Criar stream de vídeo
    AVStream *videoStream = avformat_new_stream(formatCtx, nullptr);
    if (!videoStream)
    {
        LOG_ERROR("MediaMuxer: Failed to create video stream");
        avio_context_free(&formatCtx->pb);
        av_free(const_cast<char *>(formatCtx->url));
        avformat_free_context(formatCtx);
        return false;
    }
    videoStream->id = formatCtx->nb_streams - 1;

    // Copiar parâmetros do codec para o stream
    if (avcodec_parameters_from_context(videoStream->codecpar, videoCtx) < 0)
    {
        LOG_ERROR("MediaMuxer: Failed to copy video codec parameters");
        avio_context_free(&formatCtx->pb);
        av_free(const_cast<char *>(formatCtx->url));
        avformat_free_context(formatCtx);
        return false;
    }

    videoStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    videoStream->codecpar->codec_id = videoCtx->codec_id;
    videoStream->time_base = videoCtx->time_base;

    if (videoStream->codecpar->width == 0 || videoStream->codecpar->height == 0)
    {
        videoStream->codecpar->width = videoCtx->width;
        videoStream->codecpar->height = videoCtx->height;
    }

    m_videoStream = videoStream;

    // Criar stream de áudio
    AVStream *audioStream = avformat_new_stream(formatCtx, nullptr);
    if (!audioStream)
    {
        LOG_ERROR("MediaMuxer: Failed to create audio stream");
        avio_context_free(&formatCtx->pb);
        av_free(const_cast<char *>(formatCtx->url));
        avformat_free_context(formatCtx);
        return false;
    }
    audioStream->id = formatCtx->nb_streams - 1;

    // Copiar parâmetros do codec para o stream
    if (avcodec_parameters_from_context(audioStream->codecpar, audioCtx) < 0)
    {
        LOG_ERROR("MediaMuxer: Failed to copy audio codec parameters");
        avio_context_free(&formatCtx->pb);
        av_free(const_cast<char *>(formatCtx->url));
        avformat_free_context(formatCtx);
        return false;
    }

    // Garantir que codec_type e codec_id estão corretos
    audioStream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    audioStream->codecpar->codec_id = audioCtx->codec_id;
    audioStream->time_base = audioCtx->time_base;
    m_audioStream = audioStream;

    // Para VP8/VP9, enviar frame dummy para gerar extradata
    if (videoCtx->codec_id == AV_CODEC_ID_VP8 || videoCtx->codec_id == AV_CODEC_ID_VP9)
    {
        AVFrame *dummyFrame = av_frame_alloc();
        if (dummyFrame)
        {
            dummyFrame->format = videoCtx->pix_fmt;
            dummyFrame->width = videoCtx->width;
            dummyFrame->height = videoCtx->height;
            if (av_frame_get_buffer(dummyFrame, 32) >= 0)
            {
                // Preencher com dados YUV válidos (frame preto)
                memset(dummyFrame->data[0], 0, dummyFrame->linesize[0] * dummyFrame->height);
                if (dummyFrame->data[1])
                    memset(dummyFrame->data[1], 128, dummyFrame->linesize[1] * dummyFrame->height / 2);
                if (dummyFrame->data[2])
                    memset(dummyFrame->data[2], 128, dummyFrame->linesize[2] * dummyFrame->height / 2);

                dummyFrame->pts = 0;
                FFmpegCompat::setKeyFrame(dummyFrame, true);

                if (avcodec_send_frame(videoCtx, dummyFrame) >= 0)
                {
                    AVPacket *pkt = av_packet_alloc();
                    if (pkt)
                    {
                        while (avcodec_receive_packet(videoCtx, pkt) >= 0)
                        {
                            av_packet_unref(pkt);
                        }
                        av_packet_free(&pkt);
                    }

                    if (avcodec_parameters_from_context(videoStream->codecpar, videoCtx) >= 0)
                    {
                        videoStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
                        videoStream->codecpar->codec_id = videoCtx->codec_id;
                    }
                }
            }
            av_frame_free(&dummyFrame);
        }
    }

    // Escrever header do formato
    // CRITICAL: avformat_write_header may change stream->time_base!
    // We need to use the time_base AFTER writing the header
    if (avformat_write_header(formatCtx, nullptr) < 0)
    {
        LOG_ERROR("MediaMuxer: Failed to write format header");
        avio_context_free(&formatCtx->pb);
        av_free(const_cast<char *>(formatCtx->url));
        avformat_free_context(formatCtx);
        return false;
    }
    
    // Log actual time_base after header (FFmpeg may have changed it)
    if (videoStream)
    {
        LOG_INFO("MediaMuxer: Video stream time_base after header: " + 
                 std::to_string(videoStream->time_base.num) + "/" + 
                 std::to_string(videoStream->time_base.den) + 
                 " (codec: " + std::to_string(videoCtx->time_base.num) + "/" + 
                 std::to_string(videoCtx->time_base.den) + ")");
    }
    if (audioStream)
    {
        LOG_INFO("MediaMuxer: Audio stream time_base after header: " + 
                 std::to_string(audioStream->time_base.num) + "/" + 
                 std::to_string(audioStream->time_base.den) + 
                 " (codec: " + std::to_string(audioCtx->time_base.num) + "/" + 
                 std::to_string(audioCtx->time_base.den) + ")");
    }

    m_muxerContext = formatCtx;
    m_headerWritten = false;

    return true;
}

void MediaMuxer::captureFormatHeader(const uint8_t *buf, size_t buf_size)
{
    std::lock_guard<std::mutex> lock(m_headerMutex);
    if (!m_headerWritten && buf_size > 0)
    {
        size_t remaining = m_headerCaptureSize - m_formatHeader.size();
        if (remaining > 0)
        {
            size_t toCapture = std::min(remaining, buf_size);
            m_formatHeader.insert(m_formatHeader.end(), buf, buf + toCapture);
            if (m_formatHeader.size() >= m_headerCaptureSize)
            {
                m_headerWritten = true;
            }
        }
    }
}

int MediaMuxer::callWriteCallback(const uint8_t *buf, size_t buf_size)
{
    if (m_writeCallback)
    {
        return m_writeCallback(buf, buf_size);
    }
    return -1;
}

bool MediaMuxer::muxPacket(const MediaEncoder::EncodedPacket &packet)
{
    if (!m_initialized || !m_muxerContext || !m_writeCallback)
    {
        return false;
    }

    AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_muxerContext);
    if (!formatCtx || !formatCtx->pb)
    {
        return false;
    }

    // Criar AVPacket a partir do EncodedPacket
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
    {
        LOG_ERROR("MediaMuxer: Failed to allocate packet");
        return false;
    }

    if (packet.data.empty() || packet.data.size() > static_cast<size_t>(INT_MAX))
    {
        LOG_ERROR("MediaMuxer: Invalid packet data size");
        av_packet_free(&pkt);
        return false;
    }

    AVBufferRef *buf = av_buffer_alloc(packet.data.size());
    if (!buf)
    {
        LOG_ERROR("MediaMuxer: Failed to allocate AVBufferRef");
        av_packet_free(&pkt);
        return false;
    }

    memcpy(buf->data, packet.data.data(), packet.data.size());

    if (packet.data.size() > 0)
    {
        if (buf->data[0] != packet.data[0] ||
            (packet.data.size() > 1 && buf->data[packet.data.size() - 1] != packet.data[packet.data.size() - 1]))
        {
            LOG_ERROR("MediaMuxer: Data corruption in packet copy");
            av_buffer_unref(&buf);
            av_packet_free(&pkt);
            return false;
        }
    }

    pkt->buf = av_buffer_ref(buf);
    if (!pkt->buf)
    {
        LOG_ERROR("MediaMuxer: Failed to create packet buffer reference");
        av_buffer_unref(&buf);
        av_packet_free(&pkt);
        return false;
    }

    pkt->data = pkt->buf->data;
    pkt->size = static_cast<int>(packet.data.size());
    av_buffer_unref(&buf);

    pkt->pts = (packet.pts != -1) ? packet.pts : AV_NOPTS_VALUE;
    pkt->dts = (packet.dts != -1) ? packet.dts : AV_NOPTS_VALUE;

    if (packet.isVideo && packet.isKeyframe)
    {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }

    if (packet.isVideo)
    {
        AVStream *videoStream = static_cast<AVStream *>(m_videoStream);
        if (videoStream)
        {
            pkt->stream_index = videoStream->index;
        }
        else
        {
            av_packet_free(&pkt);
            return false;
        }
    }
    else
    {
        AVStream *audioStream = static_cast<AVStream *>(m_audioStream);
        if (audioStream)
        {
            pkt->stream_index = audioStream->index;
        }
        else
        {
            av_packet_free(&pkt);
            return false;
        }
    }

    convertPTS(packet, pkt->pts, pkt->dts);

    if (pkt->dts == AV_NOPTS_VALUE)
    {
        if (pkt->pts != AV_NOPTS_VALUE)
        {
            pkt->dts = pkt->pts;
        }
        else
        {
            LOG_ERROR("MediaMuxer: Both PTS and DTS are invalid");
            av_packet_free(&pkt);
            return false;
        }
    }

    if (pkt->pts != AV_NOPTS_VALUE && pkt->dts > pkt->pts)
    {
        pkt->dts = pkt->pts;
    }

    ensureMonotonicPTS(pkt->pts, pkt->dts, packet.isVideo);

    {
        std::lock_guard<std::mutex> lock(m_muxMutex);
        int ret = av_interleaved_write_frame(formatCtx, pkt);
        if (ret < 0)
        {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("MediaMuxer: Failed to write packet: " + std::string(errbuf));
            av_packet_free(&pkt);
            return false;
        }
    }

    av_packet_free(&pkt);
    return true;
}

void MediaMuxer::convertPTS(const MediaEncoder::EncodedPacket &packet, int64_t &pts, int64_t &dts)
{
    AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_muxerContext);
    if (!formatCtx)
    {
        return;
    }

    AVStream *stream = packet.isVideo ? static_cast<AVStream *>(m_videoStream) : static_cast<AVStream *>(m_audioStream);
    if (!stream)
    {
        return;
    }

    AVCodecContext *codecCtx = packet.isVideo ? static_cast<AVCodecContext *>(m_videoCodecContext) : static_cast<AVCodecContext *>(m_audioCodecContext);
    if (!codecCtx)
    {
        return;
    }

    AVRational codecTimeBase = codecCtx->time_base;
    AVRational streamTimeBase = stream->time_base;

    // Log time_base mismatch occasionally for debugging
    static int timeBaseLogCounter = 0;
    timeBaseLogCounter++;
    if (timeBaseLogCounter == 1 || timeBaseLogCounter % 300 == 0)
    {
        LOG_INFO("MediaMuxer: PTS conversion - codec time_base: " + std::to_string(codecTimeBase.num) + "/" + std::to_string(codecTimeBase.den) +
                 ", stream time_base: " + std::to_string(streamTimeBase.num) + "/" + std::to_string(streamTimeBase.den) +
                 ", original PTS: " + std::to_string(pts));
    }

    bool needsConversion = (codecTimeBase.num != streamTimeBase.num || codecTimeBase.den != streamTimeBase.den);

    if (pts != AV_NOPTS_VALUE && pts != -1 && needsConversion)
    {
        int64_t originalPTS = pts;
        pts = av_rescale_q(pts, codecTimeBase, streamTimeBase);
        
        if (timeBaseLogCounter == 1 || timeBaseLogCounter % 300 == 0)
        {
            LOG_INFO("MediaMuxer: PTS converted - original: " + std::to_string(originalPTS) + 
                     ", converted: " + std::to_string(pts));
        }
    }

    if (dts != AV_NOPTS_VALUE && dts != -1 && needsConversion)
    {
        dts = av_rescale_q(dts, codecTimeBase, streamTimeBase);
    }
}

void MediaMuxer::ensureMonotonicPTS(int64_t &pts, int64_t &dts, bool isVideo)
{
    const int64_t AV_NOPTS_VALUE_LOCAL = AV_NOPTS_VALUE;

    std::lock_guard<std::mutex> lock(m_ptsMutex);

    if (isVideo)
    {
        if (pts != AV_NOPTS_VALUE_LOCAL)
        {
            // CRITICAL: Only prevent retrocession (PTS going backwards)
            // Don't force minimum increment - use PTS as calculated for correct speed
            // This ensures video speed matches reality based on timestamps
            if (m_lastVideoPTS >= 0 && pts <= m_lastVideoPTS)
            {
                // Log when we prevent retrocession
                static int retroLogCounter = 0;
                if (retroLogCounter++ < 5)
                {
                    LOG_WARN("MediaMuxer: Preventing PTS retrocession - last: " + std::to_string(m_lastVideoPTS) + 
                             ", calculated: " + std::to_string(pts) + ", adjusted to: " + std::to_string(m_lastVideoPTS + 1));
                }
                // PTS would go backwards - just increment by 1 to prevent retrocession
                // But don't force a large increment that would slow down the video
                pts = m_lastVideoPTS + 1;
            }
            // Otherwise, use PTS as-is for correct speed
            
            // Log PTS progression occasionally
            static int muxLogCounter = 0;
            muxLogCounter++;
            if (muxLogCounter == 1 || muxLogCounter % 300 == 0)
            {
                LOG_INFO("MediaMuxer: Video PTS - current: " + std::to_string(pts) + 
                         ", last: " + std::to_string(m_lastVideoPTS) + 
                         ", increment: " + std::to_string(pts - m_lastVideoPTS));
            }
            
            m_lastVideoPTS = pts;
        }
        if (dts != AV_NOPTS_VALUE_LOCAL)
        {
            if (m_lastVideoDTS >= 0 && dts < m_lastVideoDTS)
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

void MediaMuxer::flush()
{
    if (!m_initialized || !m_muxerContext)
    {
        return;
    }

    AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_muxerContext);
    if (formatCtx)
    {
        av_write_frame(formatCtx, nullptr); // Flush
    }
}

std::vector<uint8_t> MediaMuxer::getFormatHeader() const
{
    std::lock_guard<std::mutex> lock(m_headerMutex);
    return m_formatHeader;
}

void MediaMuxer::cleanup()
{
    if (m_muxerContext)
    {
        AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_muxerContext);
        if (formatCtx)
        {
            // Write trailer if format and IO context are valid
            if (formatCtx->oformat && formatCtx->pb)
            {
                // av_write_trailer may write to the file, ensure it's safe to call
                int ret = av_write_trailer(formatCtx);
                if (ret < 0)
                {
                    char errbuf[AV_ERROR_MAX_STRING_SIZE];
                    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
                    LOG_WARN("MediaMuxer: av_write_trailer returned error: " + std::string(errbuf));
                }
            }

            // Free IO context (this may trigger write callbacks, so file should still be open)
            if (formatCtx->pb)
            {
                avio_context_free(&formatCtx->pb);
                formatCtx->pb = nullptr;
            }

            // Free URL string if allocated
            if (formatCtx->url)
            {
                av_free(const_cast<char *>(formatCtx->url));
                formatCtx->url = nullptr;
            }

            // Free format context
            avformat_free_context(formatCtx);
        }
        m_muxerContext = nullptr;
    }

    m_videoStream = nullptr;
    m_audioStream = nullptr;

    // Não liberar codec contexts - eles pertencem ao MediaEncoder
    m_videoCodecContext = nullptr;
    m_audioCodecContext = nullptr;

    {
        std::lock_guard<std::mutex> lock(m_headerMutex);
        m_formatHeader.clear();
        m_headerWritten = false;
    }

    {
        std::lock_guard<std::mutex> lock(m_ptsMutex);
        m_lastVideoPTS = -1;
        m_lastVideoDTS = -1;
        m_lastAudioPTS = -1;
        m_lastAudioDTS = -1;
    }

    m_initialized = false;
}
