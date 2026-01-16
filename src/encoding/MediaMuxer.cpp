#include "MediaMuxer.h"
#include "../utils/Logger.h"
#include <cstring>
#include <algorithm>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h> // Para av_rescale_q
#include <libavutil/dict.h>        // Para AVDictionary
}

// Incluir FFmpegCompat.h antes para ter acesso às macros
#include "../utils/FFmpegCompat.h"

// Callback wrapper para FFmpeg (precisa ser estático)
// Diferentes versões do FFmpeg têm assinaturas diferentes:
// - FFmpeg 6.1+ (libavformat 61+): const uint8_t*
// - FFmpeg 6.0- (libavformat 60-): uint8_t* (não const)
// Usamos const uint8_t* (mais seguro) e fazemos cast quando necessário
static int writeCallback(void *opaque, const uint8_t *buf, int buf_size)
{
    if (!opaque || !buf || buf_size <= 0)
    {
        return -1;
    }

    MediaMuxer *muxer = static_cast<MediaMuxer *>(opaque);
    if (!muxer)
    {
        return -1;
    }

    // CRITICAL: Check if muxer is still initialized (avoid crashes during cleanup)
    // Return success immediately if not initialized to prevent any processing
    if (!muxer->isInitialized())
    {
        // During cleanup, return success to avoid FFmpeg errors
        // Don't call any methods that might access freed resources
        return static_cast<int>(buf_size);
    }

    // Chamar callback customizado (captureFormatHeader é chamado dentro dele se necessário)
    return muxer->callWriteCallback(buf, buf_size);
}

// Wrapper para compatibilidade com versões do FFmpeg que esperam uint8_t* (não const)
// FFmpeg 6.0 (libavformat 60) ainda usa uint8_t* (não const)
// Esta função sempre chama writeCallback, então writeCallback sempre será usada
// Usada apenas quando FFMPEG_USE_CONST_WRITE_CALLBACK não está definido
static int __attribute__((unused)) writeCallbackNonConst(void *opaque, uint8_t *buf, int buf_size)
{
    return writeCallback(opaque, const_cast<const uint8_t *>(buf), buf_size);
}

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
                            const std::string &filePath,
                            WriteCallback writeCallback,
                            size_t avioBufferSize,
                            const std::string &containerFormat)
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
    
    // Reset PTS tracking
    m_lastVideoPTS = -1;
    m_lastVideoDTS = -1;
    m_lastAudioPTS = -1;
    m_lastAudioDTS = -1;

    // Armazenar tamanho do buffer AVIO (apenas para callback, ignorado se usar filePath)
    m_avioBufferSize = (avioBufferSize > 0) ? avioBufferSize : (256 * 1024); // 256KB padrão se 0

    // Determinar formato do container
    if (!containerFormat.empty())
    {
        m_containerFormat = containerFormat;
    }
    else if (!filePath.empty())
    {
        // Detectar formato do arquivo pela extensão
        std::string ext = filePath.substr(filePath.find_last_of('.'));
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".mp4" || ext == ".m4v")
            m_containerFormat = "mp4";
        else if (ext == ".mkv")
            m_containerFormat = "matroska";
        else if (ext == ".webm")
            m_containerFormat = "webm";
        else
            m_containerFormat = "mp4"; // Default
    }
    else
    {
        m_containerFormat = "mpegts"; // Default para streaming
    }

    if (!initializeStreams(videoCodecContext, audioCodecContext, filePath, m_avioBufferSize))
    {
        // initializeStreams já faz cleanup completo em caso de erro, não precisamos chamar cleanup() novamente
        // Isso evita double free
        return false;
    }

    m_initialized = true;
    return true;
}

bool MediaMuxer::initializeStreams(void *videoCodecContext, void *audioCodecContext, const std::string &filePath, size_t avioBufferSize)
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

    // Determinar formato do container
    // Para arquivos, usar o nome do arquivo também para melhor detecção
    const char *formatName = m_containerFormat.c_str();
    const char *filename = filePath.empty() ? nullptr : filePath.c_str();
    formatCtx->oformat = av_guess_format(formatName, filename, nullptr);
    if (!formatCtx->oformat)
    {
        LOG_ERROR("MediaMuxer: Failed to guess muxer format: " + m_containerFormat + 
                  (filename ? " (filename: " + std::string(filename) + ")" : ""));
        avformat_free_context(formatCtx);
        return false;
    }
    LOG_INFO("MediaMuxer: Detected format: " + std::string(formatCtx->oformat->name) + 
             " (long_name: " + std::string(formatCtx->oformat->long_name) + ")");

    // Se filePath fornecido, usar avio_open (suporta seek, necessário para MP4)
    // Se não, usar callback customizado (para streaming)
    if (!filePath.empty())
    {
        // Usar avio_open para arquivo (suporta seek)
        formatCtx->url = av_strdup(filePath.c_str());
        if (!formatCtx->url)
        {
            LOG_ERROR("MediaMuxer: Failed to allocate file path");
            avformat_free_context(formatCtx);
            return false;
        }

        // Abrir arquivo com avio_open (permite seek)
        // IMPORTANTE: avio_open cria/trunca o arquivo, então deve ser chamado ANTES de avformat_write_header
        // AVIO_FLAG_WRITE cria/trunca o arquivo para escrita
        LOG_INFO("MediaMuxer: Opening file with avio_open: " + filePath);
        int ret = avio_open(&formatCtx->pb, filePath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0)
        {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("MediaMuxer: Failed to open output file: " + filePath + " - " + std::string(errbuf));
            av_free(const_cast<char *>(formatCtx->url));
            avformat_free_context(formatCtx);
            return false;
        }
        LOG_INFO("MediaMuxer: File opened successfully, pb=" + std::to_string(formatCtx->pb != nullptr) + 
                 ", seekable=" + std::to_string(formatCtx->pb->seekable));
        
        // Verificar se o arquivo foi criado/truncado corretamente
        // O arquivo deve estar vazio (ou truncado) antes de escrever o header
        if (formatCtx->pb->pos != 0)
        {
            LOG_WARN("MediaMuxer: File position is not 0 after opening: " + std::to_string(formatCtx->pb->pos));
        }
    }
    else
    {
        // Usar callback customizado para streaming
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
    }

    // Não usar movflags especiais - deixar FFmpeg usar padrão
    // O FFmpeg escreve o moov atom corretamente no final com av_write_trailer()
    m_formatOptions = nullptr;

    // Criar stream de vídeo
    AVStream *videoStream = avformat_new_stream(formatCtx, nullptr);
    if (!videoStream)
    {
        LOG_ERROR("MediaMuxer: Failed to create video stream");
        bool isFile = (formatCtx->url && strcmp(formatCtx->url, "pipe:") != 0);
        if (formatCtx->pb)
        {
            if (isFile)
            {
                avio_closep(&formatCtx->pb);
            }
            else
            {
                avio_context_free(&formatCtx->pb);
            }
        }
        if (formatCtx->url)
        {
            av_free(const_cast<char *>(formatCtx->url));
        }
        avformat_free_context(formatCtx);
        return false;
    }
    videoStream->id = formatCtx->nb_streams - 1;

    // Copiar parâmetros do codec para o stream
    if (avcodec_parameters_from_context(videoStream->codecpar, videoCtx) < 0)
    {
        LOG_ERROR("MediaMuxer: Failed to copy video codec parameters");
        bool isFile = (formatCtx->url && strcmp(formatCtx->url, "pipe:") != 0);
        if (formatCtx->pb)
        {
            if (isFile)
            {
                avio_closep(&formatCtx->pb);
            }
            else
            {
                avio_context_free(&formatCtx->pb);
            }
        }
        if (formatCtx->url)
        {
            av_free(const_cast<char *>(formatCtx->url));
        }
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
        bool isFile = (formatCtx->url && strcmp(formatCtx->url, "pipe:") != 0);
        if (formatCtx->pb)
        {
            if (isFile)
            {
                avio_closep(&formatCtx->pb);
            }
            else
            {
                avio_context_free(&formatCtx->pb);
            }
        }
        if (formatCtx->url)
        {
            av_free(const_cast<char *>(formatCtx->url));
        }
        avformat_free_context(formatCtx);
        return false;
    }
    audioStream->id = formatCtx->nb_streams - 1;

    // Copiar parâmetros do codec para o stream
    if (avcodec_parameters_from_context(audioStream->codecpar, audioCtx) < 0)
    {
        LOG_ERROR("MediaMuxer: Failed to copy audio codec parameters");
        bool isFile = (formatCtx->url && strcmp(formatCtx->url, "pipe:") != 0);
        if (formatCtx->pb)
        {
            if (isFile)
            {
                avio_closep(&formatCtx->pb);
            }
            else
            {
                avio_context_free(&formatCtx->pb);
            }
        }
        if (formatCtx->url)
        {
            av_free(const_cast<char *>(formatCtx->url));
        }
        avformat_free_context(formatCtx);
        return false;
    }

    // Garantir que codec_type e codec_id estão corretos
    audioStream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    audioStream->codecpar->codec_id = audioCtx->codec_id;
    audioStream->time_base = audioCtx->time_base;
    m_audioStream = audioStream;

    // Copiar parâmetros do codec de vídeo para o stream
    // Com AV_CODEC_FLAG_GLOBAL_HEADER, o FFmpeg já coloca o extradata no codec context
    // e avcodec_parameters_from_context copia isso automaticamente
    if (avcodec_parameters_from_context(videoStream->codecpar, videoCtx) < 0)
    {
        LOG_ERROR("MediaMuxer: Failed to copy video codec parameters");
        bool isFile = (formatCtx->url && strcmp(formatCtx->url, "pipe:") != 0);
        if (formatCtx->pb)
        {
            if (isFile)
            {
                avio_closep(&formatCtx->pb);
            }
            else
            {
                avio_context_free(&formatCtx->pb);
            }
        }
        if (formatCtx->url)
        {
            av_free(const_cast<char *>(formatCtx->url));
        }
        avformat_free_context(formatCtx);
        return false;
    }
    
    // Garantir que codec_type e codec_id estão corretos
    videoStream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    videoStream->codecpar->codec_id = videoCtx->codec_id;
    
    LOG_INFO("MediaMuxer: Video codecpar - codec_id: " + std::to_string(videoStream->codecpar->codec_id) +
             ", width: " + std::to_string(videoStream->codecpar->width) +
             ", height: " + std::to_string(videoStream->codecpar->height) +
             ", extradata_size: " + std::to_string(videoStream->codecpar->extradata_size));

    // Escrever header do formato
    // CRITICAL: avformat_write_header may change stream->time_base!
    // We need to use the time_base AFTER writing the header
    // IMPORTANTE: avformat_write_header escreve o ftyp box e outros metadados iniciais
    LOG_INFO("MediaMuxer: Writing format header for " + m_containerFormat + "...");
    if (formatCtx->pb)
    {
        LOG_INFO("MediaMuxer: Before header - pb position: " + std::to_string(formatCtx->pb->pos) + 
                 ", seekable: " + std::to_string(formatCtx->pb->seekable));
    }
    
    // Log codec parameters para debug
    if (videoStream && videoStream->codecpar)
    {
        LOG_INFO("MediaMuxer: Video codecpar - codec_id: " + std::to_string(videoStream->codecpar->codec_id) +
                 ", width: " + std::to_string(videoStream->codecpar->width) +
                 ", height: " + std::to_string(videoStream->codecpar->height) +
                 ", extradata_size: " + std::to_string(videoStream->codecpar->extradata_size));
    }
    if (audioStream && audioStream->codecpar)
    {
        // Obter número de canais de forma compatível com diferentes versões do FFmpeg
        int channels = 0;
        #if LIBAVCODEC_VERSION_MAJOR >= 59
            channels = audioStream->codecpar->ch_layout.nb_channels;
        #else
            channels = audioStream->codecpar->channels;
        #endif
        
        LOG_INFO("MediaMuxer: Audio codecpar - codec_id: " + std::to_string(audioStream->codecpar->codec_id) +
                 ", sample_rate: " + std::to_string(audioStream->codecpar->sample_rate) +
                 ", channels: " + std::to_string(channels) +
                 ", extradata_size: " + std::to_string(audioStream->codecpar->extradata_size));
    }
    
    AVDictionary *optsPtr = static_cast<AVDictionary *>(m_formatOptions);
    int headerRet = avformat_write_header(formatCtx, &optsPtr);
    if (headerRet < 0)
    {
        char errbuf[256];
        av_strerror(headerRet, errbuf, sizeof(errbuf));
        LOG_ERROR("MediaMuxer: Failed to write format header for " + m_containerFormat + ": " + std::string(errbuf));
        
        // Cleanup: usar avio_closep se arquivo foi aberto com avio_open, avio_context_free se foi callback
        bool isFile = (formatCtx->url && strcmp(formatCtx->url, "pipe:") != 0);
        if (formatCtx->pb)
        {
            if (isFile)
            {
                avio_closep(&formatCtx->pb); // Fechar arquivo aberto com avio_open
            }
            else
            {
                avio_context_free(&formatCtx->pb); // Liberar contexto de callback
            }
        }
        if (formatCtx->url)
        {
            av_free(const_cast<char *>(formatCtx->url));
        }
        avformat_free_context(formatCtx);
        return false;
    }
    if (formatCtx->pb)
    {
        LOG_INFO("MediaMuxer: After header - pb position: " + std::to_string(formatCtx->pb->pos) + 
                 ", bytes written: " + std::to_string(formatCtx->pb->pos));
        
        // Flush buffer AVIO após escrever header para garantir que dados sejam escritos
        avio_flush(formatCtx->pb);
        LOG_INFO("MediaMuxer: Flushed AVIO buffer after header write");
    }
    LOG_INFO("MediaMuxer: Format header written successfully");

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
    // Não capturar durante cleanup
    if (!m_initialized)
    {
        return;
    }

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
    // Se não está inicializado, retornar sucesso para evitar erros do FFmpeg
    if (!m_initialized)
    {
        return static_cast<int>(buf_size);
    }

    if (m_writeCallback)
    {
        return m_writeCallback(buf, buf_size);
    }
    return static_cast<int>(buf_size); // Retornar sucesso se não há callback
}

bool MediaMuxer::muxPacket(const MediaEncoder::EncodedPacket &packet)
{
    if (!m_initialized || !m_muxerContext)
    {
        return false;
    }

    AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_muxerContext);
    if (!formatCtx || !formatCtx->pb)
    {
        return false;
    }
    
    // Para streaming (pipe:), precisamos de callback. Para arquivo, não (FFmpeg escreve diretamente)
    if (formatCtx->url && strcmp(formatCtx->url, "pipe:") == 0)
    {
        // Usando callback (streaming) - precisa estar disponível
        if (!m_writeCallback)
        {
            return false;
        }
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

void MediaMuxer::finalize()
{
    // Finalizar escrita antes de fechar arquivo
    if (m_initialized && m_muxerContext)
    {
        AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_muxerContext);
        if (formatCtx && formatCtx->pb && formatCtx->oformat)
        {
            bool isFile = (formatCtx->url && strcmp(formatCtx->url, "pipe:") != 0);
            
            LOG_INFO("MediaMuxer: Finalizing " + m_containerFormat + " file (isFile=" + 
                     std::to_string(isFile) + ", url=" + (formatCtx->url ? formatCtx->url : "null") + ")");

            // Flush final de pacotes pendentes
            av_interleaved_write_frame(formatCtx, nullptr);

            // Flush do buffer AVIO ANTES do trailer
            if (formatCtx->pb)
            {
                avio_flush(formatCtx->pb);
            }

            // av_write_trailer() escreve metadados finais e calcula duração automaticamente
            LOG_INFO("MediaMuxer: Calling av_write_trailer()...");
            int ret = av_write_trailer(formatCtx);
            if (ret < 0)
            {
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("MediaMuxer: Failed to write trailer: " + std::string(errbuf));
            }
            else
            {
                LOG_INFO("MediaMuxer: av_write_trailer() completed successfully");
            }

            // Flush final do buffer AVIO APÓS o trailer
            if (formatCtx->pb)
            {
                avio_flush(formatCtx->pb);
            }

            // Fechar arquivo se foi aberto com avio_open (não fechar se for pipe:)
            if (isFile && formatCtx->pb)
            {
                LOG_INFO("MediaMuxer: Closing file with avio_closep()...");
                avio_closep(&formatCtx->pb);
                LOG_INFO("MediaMuxer: File closed successfully");
            }
        }
        else
        {
            LOG_ERROR("MediaMuxer: Cannot finalize - formatCtx=" + 
                     std::to_string(formatCtx != nullptr) + 
                     ", pb=" + std::to_string(formatCtx && formatCtx->pb != nullptr) +
                     ", oformat=" + std::to_string(formatCtx && formatCtx->oformat != nullptr));
        }
    }
    else
    {
        LOG_ERROR("MediaMuxer: Cannot finalize - m_initialized=" + std::to_string(m_initialized) +
                 ", m_muxerContext=" + std::to_string(m_muxerContext != nullptr));
    }
}

void MediaMuxer::cleanup()
{
    // SIMPLIFICADO: Apenas marcar como não inicializado
    // Não liberar recursos FFmpeg - deixar na memória para evitar crashes
    m_initialized = false;

    // Limpar callback para evitar chamadas
    WriteCallback emptyCallback;
    m_writeCallback = emptyCallback;

    // NÃO liberar m_muxerContext, formatCtx, pb, etc.
    // Deixar tudo na memória para evitar crashes durante cleanup
}
