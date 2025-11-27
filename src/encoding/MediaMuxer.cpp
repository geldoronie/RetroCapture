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

MediaMuxer::MediaMuxer()
{
}

MediaMuxer::~MediaMuxer()
{
    cleanup();
}

bool MediaMuxer::initialize(const MediaEncoder::VideoConfig &videoConfig,
                            const MediaEncoder::AudioConfig &audioConfig,
                            void *videoCodecContext,
                            void *audioCodecContext,
                            WriteCallback writeCallback)
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

    if (!initializeStreams(videoCodecContext, audioCodecContext))
    {
        cleanup();
        return false;
    }

    m_initialized = true;
    return true;
}

bool MediaMuxer::initializeStreams(void *videoCodecContext, void *audioCodecContext)
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

    // Configurar callback de escrita
    formatCtx->pb = avio_alloc_context(
        static_cast<unsigned char *>(av_malloc(1024 * 1024)), 1024 * 1024,
        1, this, nullptr, writeCallback, nullptr);
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

    // Garantir que codec_type e codec_id estão corretos
    videoStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    videoStream->codecpar->codec_id = videoCtx->codec_id;
    videoStream->time_base = videoCtx->time_base;

    // CRÍTICO: Garantir que width e height estão definidos no codecpar
    // Isso é necessário para o decoder identificar o stream corretamente
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
                dummyFrame->flags |= AV_FRAME_FLAG_KEY;

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
    if (avformat_write_header(formatCtx, nullptr) < 0)
    {
        LOG_ERROR("MediaMuxer: Failed to write format header");
        avio_context_free(&formatCtx->pb);
        av_free(const_cast<char *>(formatCtx->url));
        avformat_free_context(formatCtx);
        return false;
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

    // CRÍTICO: Copiar dados do pacote de forma segura
    // Validar tamanho antes de copiar para evitar corrupção
    if (packet.data.empty() || packet.data.size() > static_cast<size_t>(INT_MAX))
    {
        LOG_ERROR("MediaMuxer: Invalid packet data size: " + std::to_string(packet.data.size()));
        av_packet_free(&pkt);
        return false;
    }

    // CRÍTICO: Usar av_buffer_alloc para criar buffer gerenciado pelo FFmpeg
    // Isso garante que o FFmpeg gerencie a memória corretamente e previne corrupção
    AVBufferRef *buf = av_buffer_alloc(packet.data.size());
    if (!buf)
    {
        LOG_ERROR("MediaMuxer: Failed to allocate AVBufferRef");
        av_packet_free(&pkt);
        return false;
    }

    // Copiar dados para o buffer gerenciado pelo FFmpeg
    memcpy(buf->data, packet.data.data(), packet.data.size());

    // CRÍTICO: Validar que os dados foram copiados corretamente
    // Para H.264, verificar também o formato dos NAL units (devem começar com 0x00 0x00 0x00 0x01 ou 0x00 0x00 0x01)
    if (packet.data.size() > 0)
    {
        // Verificar primeiros e últimos bytes para detectar corrupção básica
        if (buf->data[0] != packet.data[0] ||
            (packet.data.size() > 1 && buf->data[packet.data.size() - 1] != packet.data[packet.data.size() - 1]))
        {
            LOG_ERROR("MediaMuxer: Data corruption detected in packet copy! Discarding packet.");
            av_buffer_unref(&buf);
            av_packet_free(&pkt);
            return false;
        }

        // Para pacotes de vídeo H.264, validar formato básico dos NAL units
        if (packet.isVideo && packet.data.size() >= 4)
        {
            // Verificar se começa com start code (0x00 0x00 0x00 0x01 ou 0x00 0x00 0x01)
            bool hasValidStartCode = false;
            if (packet.data[0] == 0x00 && packet.data[1] == 0x00)
            {
                if ((packet.data[2] == 0x00 && packet.data[3] == 0x01) ||
                    (packet.data[2] == 0x01))
                {
                    hasValidStartCode = true;
                }
            }

            // Se não tem start code válido, pode ser formato AVCC (sem start codes)
            // Nesse caso, o primeiro byte deve ser o tipo de NAL unit (bits 0-4)
            if (!hasValidStartCode)
            {
                uint8_t nalType = packet.data[0] & 0x1F;
                // Tipos válidos de NAL unit: 1-23 para H.264
                if (nalType == 0 || nalType > 23)
                {
                    LOG_WARN("MediaMuxer: Suspicious NAL unit type: " + std::to_string(nalType) +
                             " (first bytes: " + std::to_string(packet.data[0]) + " " +
                             std::to_string(packet.data[1]) + " " + std::to_string(packet.data[2]) +
                             " " + std::to_string(packet.data[3]) + ")");
                }
            }
        }
    }

    // CRÍTICO: Associar o buffer ao pacote corretamente
    // O av_buffer_alloc já criou um AVBufferRef, precisamos apenas associá-lo ao pacote
    // Usar av_buffer_ref para incrementar a referência antes de associar
    pkt->buf = av_buffer_ref(buf);
    if (!pkt->buf)
    {
        LOG_ERROR("MediaMuxer: Failed to create packet buffer reference");
        av_buffer_unref(&buf);
        av_packet_free(&pkt);
        return false;
    }

    // CRÍTICO: Configurar data e size do pacote
    // IMPORTANTE: Após av_buffer_ref, o pkt->buf já tem sua própria referência
    // Mas precisamos garantir que pkt->data aponte para os dados corretos
    // O buf->data pode ser inválido após av_buffer_unref, então usar pkt->buf->data
    pkt->data = pkt->buf->data;
    pkt->size = static_cast<int>(packet.data.size());

    // Validar que os dados estão corretos após associar
    if (pkt->size > 0 && pkt->data)
    {
        // Verificar se os dados foram preservados corretamente
        if (pkt->data[0] != packet.data[0] ||
            (pkt->size > 1 && pkt->data[pkt->size - 1] != packet.data[packet.data.size() - 1]))
        {
            LOG_ERROR("MediaMuxer: Data corruption detected after buffer association! Discarding packet.");
            av_packet_free(&pkt);
            return false;
        }
    }

    // Liberar a referência original (o pacote agora tem sua própria via pkt->buf)
    av_buffer_unref(&buf);

    // Configurar PTS/DTS
    pkt->pts = (packet.pts != -1) ? packet.pts : AV_NOPTS_VALUE;
    pkt->dts = (packet.dts != -1) ? packet.dts : AV_NOPTS_VALUE;

    // CRÍTICO: Configurar flag de keyframe para vídeo
    // Isso é essencial para MPEG-TS saber quando enviar SPS/PPS e marcar corretamente os pacotes
    if (packet.isVideo && packet.isKeyframe)
    {
        pkt->flags |= AV_PKT_FLAG_KEY;
    }

    // Configurar stream_index
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

    // Converter PTS/DTS do time_base do codec para time_base do stream
    convertPTS(packet, pkt->pts, pkt->dts);

    // Garantir que DTS seja válido
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

    // Garantir DTS <= PTS
    if (pkt->pts != AV_NOPTS_VALUE && pkt->dts > pkt->pts)
    {
        pkt->dts = pkt->pts;
    }

    // CRÍTICO: Garantir monotonicidade no muxer APÓS conversão
    // Mas não forçar saltos muito grandes - isso causa DTS discontinuity
    // O ensureMonotonicPTS deve apenas garantir que não há regressão, não criar saltos grandes
    ensureMonotonicPTS(pkt->pts, pkt->dts, packet.isVideo);

    // CRÍTICO: Proteger av_interleaved_write_frame com mutex (não é thread-safe)
    // Isso previne corrupção de pacotes quando múltiplas threads tentam muxar simultaneamente
    // IMPORTANTE: O writeCallback será chamado DENTRO deste lock, então deve ser MUITO rápido
    // Se o callback bloquear por muito tempo, pode causar corrupção de dados no FFmpeg
    {
        std::lock_guard<std::mutex> lock(m_muxMutex);

        // Muxar pacote
        // O FFmpeg chamará o writeCallback durante esta operação
        // O callback DEVE ser rápido para não bloquear o FFmpeg e causar corrupção
        int ret = av_interleaved_write_frame(formatCtx, pkt);
        if (ret < 0)
        {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("MediaMuxer: Failed to write packet (stream=" + std::to_string(pkt->stream_index) +
                      ", pts=" + std::to_string(pkt->pts) + ", dts=" + std::to_string(pkt->dts) +
                      ", size=" + std::to_string(pkt->size) + "): " + std::string(errbuf));
            av_packet_free(&pkt);
            return false;
        }
    }

    av_packet_free(&pkt);
    return true;
}

void MediaMuxer::convertPTS(const MediaEncoder::EncodedPacket &packet, int64_t &pts, int64_t &dts)
{
    // CRÍTICO: PTS/DTS vêm do MediaEncoder no time_base do codec
    // Precisamos converter para o time_base do stream no muxer
    // SEM essa conversão, os timestamps estão errados e causam corrupção no MPEG-TS

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

    // Obter time_base do codec
    AVCodecContext *codecCtx = packet.isVideo ? static_cast<AVCodecContext *>(m_videoCodecContext) : static_cast<AVCodecContext *>(m_audioCodecContext);

    if (!codecCtx)
    {
        return;
    }

    // Verificar se os time_bases são diferentes antes de converter
    // Se forem iguais, não precisa converter (evita erros de arredondamento e DTS discontinuity)
    AVRational codecTimeBase = codecCtx->time_base;
    AVRational streamTimeBase = stream->time_base;

    bool needsConversion = (codecTimeBase.num != streamTimeBase.num ||
                            codecTimeBase.den != streamTimeBase.den);

    // Converter PTS do time_base do codec para time_base do stream
    if (pts != AV_NOPTS_VALUE && pts != -1 && needsConversion)
    {
        pts = av_rescale_q(pts, codecTimeBase, streamTimeBase);
    }

    // Converter DTS do time_base do codec para time_base do stream
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
            // CRÍTICO: Apenas corrigir se houver regressão pequena (erro de arredondamento)
            // Não forçar saltos grandes que causam DTS discontinuity
            if (m_lastVideoPTS >= 0 && pts < m_lastVideoPTS)
            {
                // Se a diferença for muito grande, pode ser um problema de conversão
                // Nesse caso, usar o último PTS + incremento mínimo
                int64_t diff = m_lastVideoPTS - pts;
                if (diff > 1000) // Se a diferença for muito grande, pode ser erro de conversão
                {
                    LOG_WARN("MediaMuxer: Large PTS regression detected: " + std::to_string(diff) +
                             " (last=" + std::to_string(m_lastVideoPTS) + ", current=" + std::to_string(pts) + ")");
                }
                pts = m_lastVideoPTS + 1; // Incremento mínimo
            }
            m_lastVideoPTS = pts;
        }
        if (dts != AV_NOPTS_VALUE_LOCAL)
        {
            // Mesma lógica para DTS
            if (m_lastVideoDTS >= 0 && dts < m_lastVideoDTS)
            {
                int64_t diff = m_lastVideoDTS - dts;
                if (diff > 1000)
                {
                    LOG_WARN("MediaMuxer: Large DTS regression detected: " + std::to_string(diff) +
                             " (last=" + std::to_string(m_lastVideoDTS) + ", current=" + std::to_string(dts) + ")");
                }
                dts = m_lastVideoDTS + 1; // Incremento mínimo
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
            if (formatCtx->oformat && formatCtx->pb)
            {
                av_write_trailer(formatCtx);
            }

            if (formatCtx->pb)
            {
                avio_context_free(&formatCtx->pb);
            }

            if (formatCtx->url)
            {
                av_free(const_cast<char *>(formatCtx->url));
            }

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
