#include "HTTPTSStreamer.h"
#include "../utils/Logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <errno.h>
#include <time.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

// Helper macros para simplificar verificações de erro do FFmpeg
#define CHECK_FFMPEG_RET(ret, msg)                         \
    do                                                     \
    {                                                      \
        if ((ret) < 0)                                     \
        {                                                  \
            char errbuf[256];                              \
            av_strerror((ret), errbuf, sizeof(errbuf));    \
            LOG_ERROR((msg) + std::string(": ") + errbuf); \
            cleanupFFmpeg();                               \
            return false;                                  \
        }                                                  \
    } while (0)

#define CHECK_NULL(ptr, msg) \
    do                       \
    {                        \
        if (!(ptr))          \
        {                    \
            LOG_ERROR(msg);  \
            cleanupFFmpeg(); \
            return false;    \
        }                    \
    } while (0)

#define CHECK_FFMPEG_RET_NO_CLEANUP(ret, msg)              \
    do                                                     \
    {                                                      \
        if ((ret) < 0)                                     \
        {                                                  \
            char errbuf[256];                              \
            av_strerror((ret), errbuf, sizeof(errbuf));    \
            LOG_ERROR((msg) + std::string(": ") + errbuf); \
            return false;                                  \
        }                                                  \
    } while (0)

// Static callback for AVIO write
static int avioWriteCallback(void *opaque, const uint8_t *buf, int buf_size)
{
    HTTPTSStreamer *self = static_cast<HTTPTSStreamer *>(opaque);
    if (!self || !buf || buf_size <= 0)
    {
        return -1;
    }
    // Write to output buffer and send to all clients
    // Retornar o número de bytes escritos para indicar sucesso
    int written = self->writeToClients(buf, buf_size);
    return written > 0 ? written : -1;
}

HTTPTSStreamer::HTTPTSStreamer()
{
    // Initialize FFmpeg structures
    m_ffmpeg = FFmpegContext{};
    // Inicializar PTS de áudio e vídeo para evitar problemas de sincronização
    m_ffmpeg.videoPts = 0;
    m_ffmpeg.audioPts = 0;
    m_ffmpeg.startTime = 0;
    m_ffmpeg.audioSamplesProcessed = 0;
    m_ffmpeg.videoFrameCount = 0;
    m_ffmpeg.audioFrameCount = 0;
    // Resetar variáveis estáticas de continuidade de PTS ao inicializar
    // (será resetado na primeira chamada de encodeAudioFrame)
}

HTTPTSStreamer::~HTTPTSStreamer()
{
    cleanup();
}

bool HTTPTSStreamer::initialize(uint16_t port, uint32_t width, uint32_t height, uint32_t fps)
{
    m_port = port;
    m_width = width;
    m_height = height;
    m_fps = fps;

    // Calcular buffer de áudio automaticamente baseado no tempo de 1 frame de vídeo
    // Buffer de vídeo = 1 frame, então buffer de áudio deve ser equivalente em tempo
    // Tempo de 1 frame de vídeo = 1/fps segundos
    // Frames de áudio necessários = (1/fps) * sampleRate / audioFrameSize
    // Adicionar um pequeno buffer extra (10%) para absorver variações
    updateAudioBufferSize();

    LOG_INFO("HTTP MPEG-TS Streamer inicializado: " + std::to_string(width) + "x" + std::to_string(height) +
             " @ " + std::to_string(fps) + "fps, porta " + std::to_string(port) +
             ", áudio: " + std::to_string(m_sampleRate) + "Hz, " + std::to_string(m_channels) + " canais" +
             ", buffer áudio: " + std::to_string(m_audioBufferSizeFrames) + " frames (sincronizado com vídeo)");

    return initializeFFmpeg();
}

void HTTPTSStreamer::setAudioFormat(uint32_t sampleRate, uint32_t channels)
{
    m_sampleRate = sampleRate;
    m_channels = channels;
    // Recalcular buffer de áudio quando o formato mudar
    updateAudioBufferSize();
    LOG_INFO("Formato de áudio configurado: " + std::to_string(sampleRate) + "Hz, " + std::to_string(channels) +
             " canais, buffer: " + std::to_string(m_audioBufferSizeFrames) + " frames");
}

void HTTPTSStreamer::setVideoCodec(const std::string &codecName)
{
    m_videoCodecName = codecName;
    LOG_INFO("Codec de vídeo configurado: " + codecName);
}

void HTTPTSStreamer::setAudioCodec(const std::string &codecName)
{
    m_audioCodecName = codecName;
    LOG_INFO("Codec de áudio configurado: " + codecName);
}

bool HTTPTSStreamer::initializeFFmpeg()
{
    // Initialize format context for MPEG-TS
    // Usar buffer mínimo (1 pacote TS) para forçar envio imediato
    m_ffmpeg.formatBufferSize = 188; // 1 pacote TS (188 bytes) - mínimo possível
    m_ffmpeg.formatBuffer = (uint8_t *)av_malloc(m_ffmpeg.formatBufferSize);
    if (!m_ffmpeg.formatBuffer)
    {
        LOG_ERROR("Falha ao alocar buffer para format context");
        return false;
    }

    // Criar AVIO context com write callback
    // O buffer será usado pelo FFmpeg para escrever dados
    AVIOContext *ioCtx = avio_alloc_context(
        m_ffmpeg.formatBuffer, m_ffmpeg.formatBufferSize, 1, this,
        nullptr,           // read function (not needed for output)
        avioWriteCallback, // write function
        nullptr);          // seek function (not needed)

    // Configurar para não usar buffer interno do AVIO (write imediato)
    ioCtx->write_flag = 1;
    ioCtx->direct = 1;            // Modo direto - escreve imediatamente sem buffering
    ioCtx->min_packet_size = 188; // Tamanho mínimo de pacote TS

    if (!ioCtx)
    {
        LOG_ERROR("Falha ao criar AVIO context");
        return false;
    }

    // Allocate format context
    AVFormatContext *formatCtx = nullptr;
    int ret = avformat_alloc_output_context2(&formatCtx, nullptr, "mpegts", nullptr);
    m_ffmpeg.formatCtx = formatCtx;
    if (ret < 0 || !formatCtx)
    {
        LOG_ERROR("Falha ao criar format context para MPEG-TS");
        avio_context_free(&ioCtx);
        return false;
    }
    // CHECK_FFMPEG_RET(ret, "Falha ao criar format context para MPEG-TS");
    // CHECK_NULL(formatCtx, "Falha ao criar format context para MPEG-TS");

    formatCtx->pb = ioCtx;
    // Note: We can't modify oformat->flags directly, but AVFMT_NOFILE is set automatically for custom IO

    // Initialize video codec (configurável)
    AVCodecID videoCodecID = AV_CODEC_ID_H264; // Padrão
    if (m_videoCodecName == "h264" || m_videoCodecName == "libx264")
    {
        videoCodecID = AV_CODEC_ID_H264;
    }
    else if (m_videoCodecName == "h265" || m_videoCodecName == "hevc" || m_videoCodecName == "libx265")
    {
        videoCodecID = AV_CODEC_ID_H265;
    }
    else if (m_videoCodecName == "vp8" || m_videoCodecName == "libvpx")
    {
        videoCodecID = AV_CODEC_ID_VP8;
    }
    else if (m_videoCodecName == "vp9" || m_videoCodecName == "libvpx-vp9")
    {
        videoCodecID = AV_CODEC_ID_VP9;
    }
    else
    {
        LOG_WARN("Codec de vídeo '" + m_videoCodecName + "' não reconhecido, usando H.264");
        videoCodecID = AV_CODEC_ID_H264;
    }

    const AVCodec *videoCodec = avcodec_find_encoder(videoCodecID);
    CHECK_NULL(videoCodec, "Codec de vídeo não encontrado: " + m_videoCodecName);

    AVStream *videoStream = avformat_new_stream(formatCtx, videoCodec);
    m_ffmpeg.videoStream = videoStream;
    CHECK_NULL(videoStream, "Falha ao criar video stream");

    AVCodecContext *videoCodecCtx = avcodec_alloc_context3(videoCodec);
    m_ffmpeg.videoCodecCtx = videoCodecCtx;
    CHECK_NULL(videoCodecCtx, "Falha ao alocar video codec context");

    videoCodecCtx->codec_id = videoCodecID;
    videoCodecCtx->codec_type = AVMEDIA_TYPE_VIDEO;
    videoCodecCtx->width = m_width;
    videoCodecCtx->height = m_height;
    videoCodecCtx->time_base = {1, static_cast<int>(m_fps)};
    videoCodecCtx->framerate = {static_cast<int>(m_fps), 1};
    videoCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    videoCodecCtx->bit_rate = m_videoBitrate;
    videoCodecCtx->gop_size = 1;     // GOP mínimo (1 = apenas I-frames ou I+P) para menor latência
    videoCodecCtx->max_b_frames = 0; // Desabilitar B-frames para menor latência

    // Set codec options para baixa latência
    av_opt_set(videoCodecCtx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(videoCodecCtx->priv_data, "tune", "zerolatency", 0);
    av_opt_set(videoCodecCtx->priv_data, "profile", "baseline", 0);   // Baseline profile para menor latência
    av_opt_set_int(videoCodecCtx->priv_data, "rc-lookahead", 0, 0);   // Sem lookahead
    av_opt_set_int(videoCodecCtx->priv_data, "sliced-threads", 1, 0); // Threading para menor latência

    ret = avcodec_open2(videoCodecCtx, videoCodec, nullptr);
    CHECK_FFMPEG_RET(ret, "Falha ao abrir codec H.264");

    ret = avcodec_parameters_from_context(videoStream->codecpar, videoCodecCtx);
    CHECK_FFMPEG_RET(ret, "Falha ao copiar parâmetros do codec");

    videoStream->time_base = videoCodecCtx->time_base;

    // Initialize audio codec (configurável)
    AVCodecID audioCodecID = AV_CODEC_ID_AAC; // Padrão
    if (m_audioCodecName == "aac" || m_audioCodecName == "libfdk_aac")
    {
        audioCodecID = AV_CODEC_ID_AAC;
    }
    else if (m_audioCodecName == "mp3" || m_audioCodecName == "libmp3lame")
    {
        audioCodecID = AV_CODEC_ID_MP3;
    }
    else if (m_audioCodecName == "opus" || m_audioCodecName == "libopus")
    {
        audioCodecID = AV_CODEC_ID_OPUS;
    }
    else
    {
        LOG_WARN("Codec de áudio '" + m_audioCodecName + "' não reconhecido, usando AAC");
        audioCodecID = AV_CODEC_ID_AAC;
    }

    const AVCodec *audioCodec = avcodec_find_encoder(audioCodecID);
    CHECK_NULL(audioCodec, "Codec de áudio não encontrado: " + m_audioCodecName);

    AVStream *audioStream = avformat_new_stream(formatCtx, audioCodec);
    m_ffmpeg.audioStream = audioStream;
    CHECK_NULL(audioStream, "Falha ao criar audio stream");

    AVCodecContext *audioCodecCtx = avcodec_alloc_context3(audioCodec);
    m_ffmpeg.audioCodecCtx = audioCodecCtx;
    CHECK_NULL(audioCodecCtx, "Falha ao alocar audio codec context");

    audioCodecCtx->codec_id = audioCodecID;
    audioCodecCtx->codec_type = AVMEDIA_TYPE_AUDIO;
    audioCodecCtx->sample_rate = m_sampleRate;
    // Use new channel layout API (FFmpeg 5.0+)
    av_channel_layout_default(&audioCodecCtx->ch_layout, m_channels);
    audioCodecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    audioCodecCtx->bit_rate = m_audioBitrate;
    // Time base baseado na sample rate para sincronização correta
    audioCodecCtx->time_base = {1, static_cast<int>(m_sampleRate)};
    // Configurar para baixa latência
    audioCodecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    ret = avcodec_open2(audioCodecCtx, audioCodec, nullptr);
    CHECK_FFMPEG_RET(ret, "Falha ao abrir codec AAC");

    ret = avcodec_parameters_from_context(audioStream->codecpar, audioCodecCtx);
    CHECK_FFMPEG_RET(ret, "Falha ao copiar parâmetros do audio codec");

    audioStream->time_base = audioCodecCtx->time_base;
    m_ffmpeg.audioFrameSize = audioCodecCtx->frame_size;

    // Recalcular buffer de áudio agora que temos o audioFrameSize
    updateAudioBufferSize();

    // Allocate frames
    AVFrame *videoFrame = av_frame_alloc();
    AVFrame *audioFrame = av_frame_alloc();
    m_ffmpeg.videoFrame = videoFrame;
    m_ffmpeg.audioFrame = audioFrame;

    if (!videoFrame || !audioFrame)
    {
        LOG_ERROR("Falha ao alocar frames");
        cleanupFFmpeg();
        return false;
    }
    // CHECK_NULL não funciona bem com múltiplos ponteiros, mantendo if explícito

    videoFrame->format = videoCodecCtx->pix_fmt;
    videoFrame->width = videoCodecCtx->width;
    videoFrame->height = videoCodecCtx->height;

    ret = av_frame_get_buffer(videoFrame, 32);
    CHECK_FFMPEG_RET(ret, "Falha ao alocar buffer do video frame");

    audioFrame->format = audioCodecCtx->sample_fmt;
    audioFrame->sample_rate = audioCodecCtx->sample_rate;
    av_channel_layout_copy(&audioFrame->ch_layout, &audioCodecCtx->ch_layout);
    audioFrame->nb_samples = m_ffmpeg.audioFrameSize;

    ret = av_frame_get_buffer(audioFrame, 0);
    CHECK_FFMPEG_RET(ret, "Falha ao alocar buffer do audio frame");

    // Initialize SWS context for RGB to YUV conversion
    SwsContext *swsCtx = sws_getContext(
        m_width, m_height, AV_PIX_FMT_RGB24,
        m_width, m_height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    m_ffmpeg.swsCtx = swsCtx;

    CHECK_NULL(swsCtx, "Falha ao criar SWS context");

    // Initialize SWR context for audio resampling
    AVChannelLayout inChLayout, outChLayout;
    av_channel_layout_default(&inChLayout, m_channels);
    av_channel_layout_default(&outChLayout, m_channels);
    SwrContext *swrCtx = nullptr;
    swr_alloc_set_opts2(&swrCtx,
                        &outChLayout, audioCodecCtx->sample_fmt, audioCodecCtx->sample_rate,
                        &inChLayout, AV_SAMPLE_FMT_S16, m_sampleRate,
                        0, nullptr);
    m_ffmpeg.swrCtx = swrCtx;

    if (!swrCtx)
    {
        LOG_ERROR("Falha ao criar SWR context");
        av_channel_layout_uninit(&inChLayout);
        av_channel_layout_uninit(&outChLayout);
        cleanupFFmpeg();
        return false;
    }

    ret = swr_init(swrCtx);
    if (ret < 0)
    {
        LOG_ERROR("Falha ao inicializar SWR context");
        av_channel_layout_uninit(&inChLayout);
        av_channel_layout_uninit(&outChLayout);
        cleanupFFmpeg();
        return false;
    }

    av_channel_layout_uninit(&inChLayout);
    av_channel_layout_uninit(&outChLayout);

    // Configurar opções de formato para baixa latência
    AVDictionary *opts = nullptr;
    av_dict_set_int(&opts, "muxrate", 0, 0);       // Sem limite de bitrate no muxer
    av_dict_set_int(&opts, "flush_packets", 1, 0); // Flush imediato
    av_dict_set_int(&opts, "pcr_period", 5, 0);    // PCR muito frequente (5ms) para menor latência
    av_dict_set_int(&opts, "max_delay", 0, 0);     // Sem delay máximo - enviar imediatamente

    av_dict_set(&formatCtx->metadata, "service_name", "RetroCapture", 0);
    av_dict_set(&formatCtx->metadata, "service_provider", "RetroCapture", 0);

    // Configurar start_time para sincronização (usar clock do sistema)
    // Isso garante que áudio e vídeo usem o mesmo clock base
    // IMPORTANTE: start_time será definido quando o streaming realmente começar (no start())
    // Por enquanto, apenas inicializar
    m_ffmpeg.startTime = 0;

    // Write header com opções de baixa latência
    ret = avformat_write_header(formatCtx, &opts);
    if (ret < 0)
    {
        LOG_ERROR("Falha ao escrever header MPEG-TS");
        av_dict_free(&opts);
        cleanupFFmpeg();
        return false;
    }
    av_dict_free(&opts);

    LOG_INFO("FFmpeg inicializado para MPEG-TS streaming");
    return true;
}

void HTTPTSStreamer::cleanupFFmpeg()
{
    if (m_ffmpeg.swsCtx)
    {
        sws_freeContext(static_cast<SwsContext *>(m_ffmpeg.swsCtx));
        m_ffmpeg.swsCtx = nullptr;
    }

    if (m_ffmpeg.swrCtx)
    {
        SwrContext *swrCtx = static_cast<SwrContext *>(m_ffmpeg.swrCtx);
        swr_free(&swrCtx);
        m_ffmpeg.swrCtx = nullptr;
    }

    if (m_ffmpeg.videoFrame)
    {
        AVFrame *frame = static_cast<AVFrame *>(m_ffmpeg.videoFrame);
        av_frame_free(&frame);
        m_ffmpeg.videoFrame = nullptr;
    }

    if (m_ffmpeg.audioFrame)
    {
        AVFrame *frame = static_cast<AVFrame *>(m_ffmpeg.audioFrame);
        av_frame_free(&frame);
        m_ffmpeg.audioFrame = nullptr;
    }

    if (m_ffmpeg.videoCodecCtx)
    {
        AVCodecContext *ctx = static_cast<AVCodecContext *>(m_ffmpeg.videoCodecCtx);
        avcodec_free_context(&ctx);
        m_ffmpeg.videoCodecCtx = nullptr;
    }

    if (m_ffmpeg.audioCodecCtx)
    {
        AVCodecContext *ctx = static_cast<AVCodecContext *>(m_ffmpeg.audioCodecCtx);
        avcodec_free_context(&ctx);
        m_ffmpeg.audioCodecCtx = nullptr;
    }

    if (m_ffmpeg.formatCtx)
    {
        AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_ffmpeg.formatCtx);
        if (formatCtx->pb)
        {
            avio_context_free(&formatCtx->pb);
        }
        avformat_free_context(formatCtx);
        m_ffmpeg.formatCtx = nullptr;
    }

    if (m_ffmpeg.formatBuffer)
    {
        av_free(m_ffmpeg.formatBuffer);
        m_ffmpeg.formatBuffer = nullptr;
    }
}

void HTTPTSStreamer::updateAudioBufferSize()
{
    // Calcular buffer de áudio baseado em múltiplos frames de vídeo para garantir continuidade
    // O buffer precisa ser grande o suficiente para absorver variações de timing
    // Usar ~0.5-1 segundo de vídeo como referência para o buffer de áudio
    //
    // Se audioFrameSize ainda não foi definido (durante initialize), usar estimativa padrão
    // AAC típico: 1024 samples por frame a 48kHz
    uint32_t estimatedFrameSize = m_ffmpeg.audioFrameSize > 0 ? m_ffmpeg.audioFrameSize : 1024;

    if (m_fps > 0 && m_sampleRate > 0 && estimatedFrameSize > 0)
    {
        // Calcular quantos frames de áudio correspondem a ~0.5 segundos de vídeo
        // Isso garante buffer suficiente para absorver variações sem gaps
        // Frames de áudio = (sampleRate * 0.5) / audioFrameSize
        // Usar 0.5 segundos como base (pode ser ajustado se necessário)
        double audioFramesForHalfSecond = (static_cast<double>(m_sampleRate) * 0.5) / static_cast<double>(estimatedFrameSize);
        uint32_t calculatedFrames = static_cast<uint32_t>(audioFramesForHalfSecond);

        // Garantir mínimo de 10 frames (para absorver variações) e máximo razoável
        calculatedFrames = std::max(10u, std::min(calculatedFrames, 200u));

        m_audioBufferSizeFrames = calculatedFrames;
    }
    else
    {
        // Fallback: usar valor padrão seguro (50 frames = ~1 segundo a 48kHz)
        m_audioBufferSizeFrames = 50;
    }
}

bool HTTPTSStreamer::start()
{
    if (m_active)
    {
        return true;
    }

    // Create server socket
    m_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSocket < 0)
    {
        LOG_ERROR("Falha ao criar socket HTTP");
        return false;
    }

    // Set socket options
    int opt = 1;
    setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind socket
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_port);

    if (bind(m_serverSocket, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        LOG_ERROR("Falha ao fazer bind na porta " + std::to_string(m_port));
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    // Listen
    if (listen(m_serverSocket, 5) < 0)
    {
        LOG_ERROR("Falha ao fazer listen");
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }

    // Inicializar clock de sincronização quando o streaming realmente começar
    // Isso garante que áudio e vídeo usem o mesmo clock base desde o início
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    m_ffmpeg.startTime = (int64_t)(ts.tv_sec * 1000000LL + ts.tv_nsec / 1000); // Em microsegundos

    // Configurar start_time no format context para sincronização
    AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_ffmpeg.formatCtx);
    if (formatCtx)
    {
        formatCtx->start_time = m_ffmpeg.startTime;
    }

    // Resetar PTS e contadores para começar do zero (relativo ao start_time)
    m_ffmpeg.videoPts = 0;
    m_ffmpeg.audioPts = 0;
    m_ffmpeg.audioSamplesProcessed = 0;
    m_ffmpeg.videoFrameCount = 0;
    m_ffmpeg.audioFrameCount = 0;
    // Resetar DTS para garantir monotonicidade desde o início
    m_ffmpeg.lastVideoDts = -1;
    m_ffmpeg.lastAudioDts = -1;

    // Limpar buffer de áudio para começar limpo
    {
        std::lock_guard<std::mutex> lock(m_audioMutex);
        m_audioBuffer.clear();
        // Limpar também a fila de samples
        while (!m_audioSamples.empty())
        {
            m_audioSamples.pop();
        }
    }

    m_running = true;
    m_active = true;
    m_serverThread = std::thread(&HTTPTSStreamer::serverThread, this);
    m_encodingThread = std::thread(&HTTPTSStreamer::encodingThread, this);

    LOG_INFO("HTTP MPEG-TS Streamer iniciado na porta " + std::to_string(m_port));
    return true;
}

void HTTPTSStreamer::stop()
{
    if (!m_active)
    {
        return;
    }

    // IMPORTANTE: Sinalizar parada primeiro para que as threads possam terminar
    m_running = false;
    m_active = false;

    // Close server socket to wake up accept()
    // IMPORTANTE: Fazer isso ANTES do join para garantir que accept() seja interrompido
    if (m_serverSocket >= 0)
    {
        shutdown(m_serverSocket, SHUT_RDWR); // Shutdown antes de close para garantir que accept() seja interrompido
        close(m_serverSocket);
        m_serverSocket = -1;
    }

    // Reset client count when stopping
    m_clientCount.store(0);

    // IMPORTANTE: Fazer join das threads (devem terminar rapidamente após m_running = false)
    // O shutdown do socket deve ter acordado accept(), então serverThread deve terminar rapidamente
    // O encodingThread deve terminar no próximo loop quando verificar m_running = false
    if (m_serverThread.joinable())
    {
        m_serverThread.join();
    }

    if (m_encodingThread.joinable())
    {
        m_encodingThread.join();
    }

    LOG_INFO("HTTP MPEG-TS Streamer parado");
}

bool HTTPTSStreamer::isActive() const
{
    return m_active;
}

bool HTTPTSStreamer::pushFrame(const uint8_t *data, uint32_t width, uint32_t height)
{
    if (!data || !m_active)
    {
        return false;
    }

    // IMPORTANTE: Usar try_lock para não bloquear o thread principal
    // Se não conseguir o lock imediatamente, descartar o frame (já temos um mais recente na fila)
    std::unique_lock<std::mutex> lock(m_frameMutex, std::try_to_lock);
    if (!lock.owns_lock())
    {
        // Não conseguiu o lock - encoding thread está ocupado
        // Descartar este frame (já temos um mais recente sendo processado)
        return false;
    }

    // IMPORTANTE: Para reduzir buffering, descartar frames antigos e manter apenas o mais recente
    // Isso garante que sempre processamos o frame mais atual, reduzindo latência
    // Limitar a fila a no máximo 1 frame para evitar acúmulo e buffering
    while (m_videoFrames.size() >= 1)
    {
        m_videoFrames.pop(); // Descartar frame antigo
    }

    // Store frame (copiar dados - operação rápida com lock já adquirido)
    size_t frameSize = width * height * 3;
    std::vector<uint8_t> frame(frameSize);
    memcpy(frame.data(), data, frameSize);
    m_videoFrames.push(std::move(frame));
    m_frameWidth = width;
    m_frameHeight = height;

    return true;
}

bool HTTPTSStreamer::pushAudio(const int16_t *samples, size_t sampleCount)
{
    if (!samples || !m_active || sampleCount == 0)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_audioMutex);

    // Store audio samples
    std::vector<int16_t> audioSamples(sampleCount);
    memcpy(audioSamples.data(), samples, sampleCount * sizeof(int16_t));
    m_audioSamples.push(std::move(audioSamples));

    return true;
}

std::string HTTPTSStreamer::getStreamUrl() const
{
    return "http://localhost:" + std::to_string(m_port) + "/stream";
}

uint32_t HTTPTSStreamer::getClientCount() const
{
    return m_clientCount.load();
}

void HTTPTSStreamer::cleanup()
{
    stop();
    cleanupFFmpeg();
}

void HTTPTSStreamer::serverThread()
{
    while (m_running)
    {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);

        int clientFd = accept(m_serverSocket, (struct sockaddr *)&clientAddr, &clientLen);
        if (clientFd < 0)
        {
            if (m_running)
            {
                LOG_ERROR("Falha ao aceitar conexão HTTP");
            }
            break;
        }

        // Handle client in separate thread
        std::thread clientThread(&HTTPTSStreamer::handleClient, this, clientFd);
        clientThread.detach();
    }
}

void HTTPTSStreamer::handleClient(int clientFd)
{
    // Configurar socket para baixa latência
    int flag = 1;
    setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)); // Desabilitar Nagle's algorithm

    // Reduzir buffer de envio para forçar envio imediato
    int sendBufSize = 8192; // 8KB buffer pequeno
    setsockopt(clientFd, SOL_SOCKET, SO_SNDBUF, &sendBufSize, sizeof(sendBufSize));

    // Read HTTP request FIRST, before incrementing counter
    char buffer[4096];
    ssize_t bytesRead = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
    if (bytesRead <= 0)
    {
        close(clientFd);
        return;
    }

    buffer[bytesRead] = '\0';

    // Check if it's a GET request for /stream
    std::string request(buffer);
    bool isStreamRequest = (request.find("GET /stream") != std::string::npos);

    // Only accept /stream requests, reject everything else (favicon, etc.)
    if (!isStreamRequest)
    {
        const char *response = "HTTP/1.1 404 Not Found\r\n\r\n";
        send(clientFd, response, strlen(response), 0);
        close(clientFd);
        return; // Don't count rejected requests
    }

    // Only increment counter for valid stream requests
    uint32_t count = ++m_clientCount;
    LOG_INFO("Cliente HTTP MPEG-TS conectado (total: " + std::to_string(count) + ")");

    // Add client to list only after confirming it's a valid stream request
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        m_clientSockets.push_back(clientFd);
    }

    // Send HTTP headers for MPEG-TS stream
    std::ostringstream headers;
    headers << "HTTP/1.1 200 OK\r\n";
    headers << "Content-Type: video/mp2t\r\n";
    headers << "Connection: keep-alive\r\n";
    headers << "Cache-Control: no-cache\r\n";
    headers << "\r\n";

    std::string headerStr = headers.str();
    ssize_t sent = send(clientFd, headerStr.c_str(), headerStr.length(), MSG_NOSIGNAL);
    if (sent < 0)
    {
        close(clientFd);
        uint32_t countAfter = --m_clientCount;
        {
            std::lock_guard<std::mutex> lock(m_outputMutex);
            m_clientSockets.erase(
                std::remove(m_clientSockets.begin(), m_clientSockets.end(), clientFd),
                m_clientSockets.end());
        }
        LOG_INFO("Cliente HTTP MPEG-TS desconectado (erro ao enviar headers, total: " + std::to_string(countAfter) + ")");
        return;
    }

    // Stream MPEG-TS data
    // Data is sent via avioWriteCallback when packets are muxed
    // Just keep the connection alive and check if client is still connected
    while (m_running)
    {
        // Check if client is still connected
        char test;
        if (recv(clientFd, &test, 1, MSG_PEEK | MSG_DONTWAIT) <= 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                break; // Client disconnected
            }
        }

        // Verificar se o cliente ainda está na lista (pode ter sido removido por writeToClients)
        {
            std::lock_guard<std::mutex> lock(m_outputMutex);
            auto it = std::find(m_clientSockets.begin(), m_clientSockets.end(), clientFd);
            if (it == m_clientSockets.end())
            {
                // Cliente já foi removido da lista (provavelmente por writeToClients)
                // Não decrementar contador novamente
                close(clientFd);
                return;
            }
        }

        // Sleep briefly to avoid busy-waiting
        usleep(10000); // 10ms
    }

    // Cliente desconectou - remover da lista e decrementar contador
    close(clientFd);
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        // Verificar se ainda está na lista antes de remover
        auto it = std::find(m_clientSockets.begin(), m_clientSockets.end(), clientFd);
        if (it != m_clientSockets.end())
        {
            m_clientSockets.erase(it);
            uint32_t countAfter = --m_clientCount;
            LOG_INFO("Cliente HTTP MPEG-TS desconectado (total: " + std::to_string(countAfter) + ")");
        }
        else
        {
            // Já foi removido por writeToClients, não decrementar novamente
            LOG_INFO("Cliente HTTP MPEG-TS desconectado (já removido da lista)");
        }
    }
}

void HTTPTSStreamer::encodingThread()
{
    // This will encode video and audio frames and mux them into MPEG-TS
    AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_ffmpeg.formatCtx);
    AVCodecContext *videoCodecCtx = static_cast<AVCodecContext *>(m_ffmpeg.videoCodecCtx);
    AVCodecContext *audioCodecCtx = static_cast<AVCodecContext *>(m_ffmpeg.audioCodecCtx);

    if (!formatCtx || !videoCodecCtx || !audioCodecCtx)
    {
        LOG_ERROR("FFmpeg context não inicializado no encoding thread");
        return;
    }

    while (m_running)
    {
        bool hasVideo = false;
        bool hasAudio = false;

        // Check for video frame
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            hasVideo = !m_videoFrames.empty();
        }

        // Check for audio samples (verificar tanto a fila quanto o buffer de acumulação)
        {
            std::lock_guard<std::mutex> lock(m_audioMutex);
            hasAudio = !m_audioSamples.empty() || !m_audioBuffer.empty();
        }

        // IMPORTANTE: Processar áudio e vídeo de forma balanceada para manter sincronização
        // Processar áudio primeiro se disponível, depois vídeo
        // Isso garante que o áudio não fique atrasado em relação ao vídeo
        bool processedAudio = false;
        if (hasAudio)
        {
            // Processar áudio de forma contínua para evitar gaps
            // IMPORTANTE: Processar áudio mesmo quando não há novos samples na fila
            // Isso garante que frames completos no buffer sejam processados continuamente
            {
                std::unique_lock<std::mutex> lock(m_audioMutex);
                // Processar todos os samples da fila primeiro
                size_t samplesPerFrame = m_ffmpeg.audioFrameSize * m_channels;
                // Usar tamanho configurável do buffer
                size_t maxBufferSize = samplesPerFrame * m_audioBufferSizeFrames;

                while (!m_audioSamples.empty())
                {
                    std::vector<int16_t> audioData = std::move(m_audioSamples.front());
                    m_audioSamples.pop();

                    // IMPORTANTE: Limitar tamanho do buffer para evitar perda de samples
                    // Buffer configurável para absorver variações sem descartar samples
                    // Apenas descartar se realmente estiver muito cheio (evitar perda de dados)
                    if (m_audioBuffer.size() > maxBufferSize)
                    {
                        // Descartar samples antigos apenas se realmente necessário (manter 90% do tamanho)
                        size_t keepSize = samplesPerFrame * (m_audioBufferSizeFrames * 9 / 10);
                        m_audioBuffer.erase(m_audioBuffer.begin(), m_audioBuffer.end() - keepSize);
                    }

                    // Adicionar ao buffer de acumulação
                    m_audioBuffer.insert(m_audioBuffer.end(), audioData.begin(), audioData.end());
                }

                // Processar TODOS os frames completos do buffer de acumulação
                // IMPORTANTE: Processar todos os frames disponíveis para evitar skipping e perda de informações
                // Não limitar o número de frames processados para garantir que nada seja perdido
                while (m_audioBuffer.size() >= samplesPerFrame)
                {
                    // Extrair um frame completo
                    std::vector<int16_t> frameSamples(m_audioBuffer.begin(), m_audioBuffer.begin() + samplesPerFrame);
                    m_audioBuffer.erase(m_audioBuffer.begin(), m_audioBuffer.begin() + samplesPerFrame);

                    // Processar o frame (fora do lock)
                    lock.unlock();
                    encodeAudioFrame(frameSamples.data(), frameSamples.size());
                    lock.lock();

                    processedAudio = true;
                }
            }
        }

        // Processar vídeo após áudio para manter sincronização
        // Vídeo tem latência menor, então pode ser processado depois
        if (hasVideo)
        {
            std::vector<uint8_t> frameData;
            uint32_t width, height;

            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                if (!m_videoFrames.empty())
                {
                    frameData = std::move(m_videoFrames.front());
                    m_videoFrames.pop();
                    width = m_frameWidth;
                    height = m_frameHeight;
                }
            }

            if (!frameData.empty())
            {
                encodeVideoFrame(frameData.data(), width, height);
            }
        }

        // Fazer flush após processar dados para garantir envio imediato
        // Isso é crítico para reduzir buffering no cliente
        AVFormatContext *fmtCtx = static_cast<AVFormatContext *>(m_ffmpeg.formatCtx);
        if (fmtCtx && fmtCtx->pb && (hasVideo || processedAudio))
        {
            avio_flush(fmtCtx->pb);
        }

        // IMPORTANTE: Controlar taxa de processamento para evitar enviar muito rápido
        // Se processamos vídeo, fazer um pequeno sleep para não sobrecarregar
        // Isso ajuda a manter a sincronização e evitar que o player fique esperando
        if (hasVideo || processedAudio)
        {
            // Calcular tempo esperado para este frame baseado na taxa de FPS
            // Para vídeo: 1/fps segundos por frame
            // Para áudio: samples/sample_rate segundos por frame
            // Usar o menor dos dois para não atrasar
            int64_t expectedFrameTimeUs = 0;
            if (hasVideo)
            {
                expectedFrameTimeUs = 1000000LL / m_fps; // Microsegundos por frame de vídeo
            }
            else if (processedAudio)
            {
                // Tempo de um frame de áudio
                int64_t audioFrameTimeUs = (m_ffmpeg.audioFrameSize * 1000000LL) / m_sampleRate;
                expectedFrameTimeUs = audioFrameTimeUs;
            }

            // Se processamos muito rápido, fazer um pequeno sleep para manter taxa correta
            // Mas não fazer sleep muito longo para não causar atraso
            if (expectedFrameTimeUs > 0)
            {
                // Sleep de 50% do tempo esperado para manter taxa mas não bloquear muito
                usleep(expectedFrameTimeUs / 2);
            }
        }
        else
        {
            // Se não há dados, fazer sleep muito curto para manter o loop responsivo
            // Mas sempre processar áudio se houver frames completos no buffer
            // Verificar se há frames completos de áudio no buffer mesmo sem novos samples
            bool hasCompleteAudioFrame = false;
            {
                std::lock_guard<std::mutex> lock(m_audioMutex);
                size_t samplesPerFrame = m_ffmpeg.audioFrameSize * m_channels;
                hasCompleteAudioFrame = (m_audioBuffer.size() >= samplesPerFrame);
            }

            if (!hasCompleteAudioFrame)
            {
                usleep(1000); // 1ms - manter loop responsivo
            }
            // Se há frame completo, continuar processando sem sleep
        }
    }
}

bool HTTPTSStreamer::encodeVideoFrame(const uint8_t *rgbData, uint32_t width, uint32_t height)
{
    if (!rgbData || !m_active)
    {
        return false;
    }

    AVCodecContext *videoCodecCtx = static_cast<AVCodecContext *>(m_ffmpeg.videoCodecCtx);
    AVFrame *videoFrame = static_cast<AVFrame *>(m_ffmpeg.videoFrame);
    SwsContext *swsCtx = static_cast<SwsContext *>(m_ffmpeg.swsCtx);
    AVStream *videoStream = static_cast<AVStream *>(m_ffmpeg.videoStream);

    if (!videoCodecCtx || !videoFrame || !swsCtx || !videoStream)
    {
        return false;
    }

    // Convert RGB to YUV420P
    const uint8_t *srcData[1] = {rgbData};
    int srcLinesize[1] = {static_cast<int>(width * 3)};

    int ret = sws_scale(swsCtx, srcData, srcLinesize, 0, height,
                        videoFrame->data, videoFrame->linesize);
    if (ret < 0)
    {
        LOG_ERROR("Falha ao converter RGB para YUV");
        return false;
    }

    // Set frame properties
    // IMPORTANTE: Usar contador incremental para sincronização precisa
    // O PTS deve ser baseado na quantidade de frames processados, não no tempo real
    // Isso garante sincronização perfeita mesmo quando há buffering ou variações de timing
    AVRational timeBase = videoCodecCtx->time_base;

    // Incrementar contador de frames de vídeo
    m_ffmpeg.videoFrameCount++;

    // Calcular PTS baseado no número de frames processados
    // PTS = videoFrameCount / fps no time_base do codec
    // time_base = {1, fps}, então PTS = videoFrameCount
    int64_t ptsInTimeBase = av_rescale_q(m_ffmpeg.videoFrameCount, {1, static_cast<int>(m_fps)}, timeBase);

    videoFrame->pts = ptsInTimeBase;
    m_ffmpeg.videoPts = ptsInTimeBase;

    // Garantir que o PTS não seja negativo
    if (videoFrame->pts < 0)
    {
        videoFrame->pts = 0;
        m_ffmpeg.videoPts = 0;
    }

    // Encode frame
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
    {
        return false;
    }

    ret = avcodec_send_frame(videoCodecCtx, videoFrame);
    if (ret < 0)
    {
        av_packet_free(&pkt);
        return false;
    }

    while (ret >= 0)
    {
        ret = avcodec_receive_packet(videoCodecCtx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            break;
        }
        else if (ret < 0)
        {
            av_packet_free(&pkt);
            return false;
        }

        // Mux packet
        pkt->stream_index = videoStream->index;
        // Rescalar PTS para o time_base do stream
        av_packet_rescale_ts(pkt, videoCodecCtx->time_base, videoStream->time_base);

        // Garantir que DTS <= PTS (necessário para MPEG-TS)
        if (pkt->dts == AV_NOPTS_VALUE)
        {
            pkt->dts = pkt->pts;
        }
        if (pkt->dts > pkt->pts)
        {
            pkt->dts = pkt->pts;
        }
        // Garantir que DTS não seja negativo
        if (pkt->dts < 0)
        {
            pkt->dts = pkt->pts;
        }

        // IMPORTANTE: Garantir que DTS seja monotônico (sempre aumentando estritamente)
        // O muxer MPEG-TS requer DTS estritamente crescente (não pode ser igual)
        // IMPORTANTE: Verificar ANTES de atualizar lastVideoDts
        if (m_ffmpeg.lastVideoDts >= 0)
        {
            // Calcular incremento mínimo baseado no time_base do stream
            AVRational streamTimeBase = videoStream->time_base;
            int64_t minDtsIncrement = av_rescale_q(1, {1, static_cast<int>(m_fps)}, streamTimeBase);
            if (minDtsIncrement <= 0)
            {
                minDtsIncrement = 1; // Garantir pelo menos 1
            }

            // CRÍTICO: Garantir que DTS seja sempre MAIOR que o anterior (não igual)
            // O erro "non monotonically increasing" ocorre quando DTS é igual ao anterior
            if (pkt->dts <= m_ffmpeg.lastVideoDts)
            {
                pkt->dts = m_ffmpeg.lastVideoDts + minDtsIncrement;
            }

            // Se DTS foi ajustado, garantir que PTS >= DTS
            if (pkt->pts < pkt->dts)
            {
                pkt->pts = pkt->dts;
            }
        }
        // Atualizar lastVideoDts APÓS todas as verificações
        m_ffmpeg.lastVideoDts = pkt->dts;

        muxPacket(pkt);

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return true;
}

bool HTTPTSStreamer::encodeAudioFrame(const int16_t *samples, size_t sampleCount)
{
    if (!m_active)
    {
        return false;
    }

    AVCodecContext *audioCodecCtx = static_cast<AVCodecContext *>(m_ffmpeg.audioCodecCtx);
    AVFrame *audioFrame = static_cast<AVFrame *>(m_ffmpeg.audioFrame);
    SwrContext *swrCtx = static_cast<SwrContext *>(m_ffmpeg.swrCtx);
    AVStream *audioStream = static_cast<AVStream *>(m_ffmpeg.audioStream);

    if (!audioCodecCtx || !audioFrame || !swrCtx || !audioStream)
    {
        return false;
    }

    // IMPORTANTE: Esta função agora apenas processa UM frame completo de samples
    // O encodingThread já processa todos os frames do buffer
    // Esta função é chamada apenas com um frame completo já extraído do buffer
    // Não processar o buffer aqui para evitar duplicação e skipping

    if (sampleCount == 0 || !samples)
    {
        return false;
    }

    // Verificar se temos samples suficientes para um frame completo
    size_t samplesPerFrame = m_ffmpeg.audioFrameSize * m_channels;
    if (sampleCount < samplesPerFrame)
    {
        // Não temos samples suficientes - retornar false mas não processar
        return false;
    }

    // Usar apenas os samples necessários para um frame completo
    std::vector<int16_t> frameSamples(samplesPerFrame);
    std::copy(samples, samples + samplesPerFrame, frameSamples.begin());

    // Resample and convert audio
    // IMPORTANTE: swr_convert espera samples de entrada no formato de entrada (S16)
    // e converte para o formato de saída (FLTP para AAC)
    const uint8_t *srcData[1] = {reinterpret_cast<const uint8_t *>(frameSamples.data())};
    const int srcSampleCount = static_cast<int>(m_ffmpeg.audioFrameSize); // Samples de entrada

    // Garantir que o frame está configurado corretamente
    audioFrame->nb_samples = m_ffmpeg.audioFrameSize;

    int ret = swr_convert(swrCtx, audioFrame->data, audioFrame->nb_samples,
                          srcData, srcSampleCount);
    if (ret < 0)
    {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Falha ao converter áudio: " + std::string(errbuf));
        return false;
    }

    if (ret == 0)
    {
        // Nenhum sample convertido, pode ser um problema
        return false;
    }

    // Ajustar nb_samples se necessário (geralmente ret == audioFrameSize)
    if (ret != m_ffmpeg.audioFrameSize)
    {
        audioFrame->nb_samples = ret;
    }

    // Set frame properties
    // IMPORTANTE: Usar contador incremental para sincronização precisa
    // O PTS deve ser baseado na quantidade de samples processados, não no tempo real
    // Isso garante sincronização perfeita mesmo quando há buffering ou variações de timing
    AVRational timeBase = audioCodecCtx->time_base;

    // Incrementar contador de frames de áudio
    m_ffmpeg.audioFrameCount++;

    // Calcular PTS baseado no número de samples processados
    // PTS = (audioFrameCount * audioFrameSize) / sampleRate no time_base do codec
    // time_base = {1, sampleRate}, então PTS = audioFrameCount * audioFrameSize
    int64_t totalSamples = m_ffmpeg.audioFrameCount * m_ffmpeg.audioFrameSize;
    int64_t ptsInTimeBase = av_rescale_q(totalSamples, {1, static_cast<int>(m_sampleRate)}, timeBase);

    audioFrame->pts = ptsInTimeBase;
    m_ffmpeg.audioPts = ptsInTimeBase;

    // Atualizar contador de samples processados para referência
    m_ffmpeg.audioSamplesProcessed += m_ffmpeg.audioFrameSize;

    // Garantir que o PTS não seja negativo
    if (audioFrame->pts < 0)
    {
        audioFrame->pts = 0;
        m_ffmpeg.audioPts = 0;
    }

    // Encode frame
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
    {
        return false;
    }

    ret = avcodec_send_frame(audioCodecCtx, audioFrame);
    if (ret < 0)
    {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Falha ao enviar frame de áudio para encoder: " + std::string(errbuf));
        av_packet_free(&pkt);
        return false;
    }

    bool encoded = false;
    while (ret >= 0)
    {
        ret = avcodec_receive_packet(audioCodecCtx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            break;
        }
        else if (ret < 0)
        {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Falha ao receber pacote de áudio do encoder: " + std::string(errbuf));
            av_packet_free(&pkt);
            return false;
        }

        // Mux packet
        pkt->stream_index = audioStream->index;

        // IMPORTANTE: O encoder já define PTS e DTS no time_base do codec
        // Rescalar para o time_base do stream antes de muxar
        av_packet_rescale_ts(pkt, audioCodecCtx->time_base, audioStream->time_base);

        // Garantir que DTS <= PTS (necessário para MPEG-TS)
        if (pkt->dts == AV_NOPTS_VALUE)
        {
            pkt->dts = pkt->pts;
        }
        if (pkt->dts > pkt->pts)
        {
            pkt->dts = pkt->pts;
        }
        // Garantir que DTS não seja negativo
        if (pkt->dts < 0)
        {
            pkt->dts = pkt->pts;
        }

        // IMPORTANTE: Garantir que DTS seja monotônico (sempre aumentando estritamente)
        // O muxer MPEG-TS requer DTS estritamente crescente (não pode ser igual)
        // IMPORTANTE: Verificar ANTES de atualizar lastAudioDts
        if (m_ffmpeg.lastAudioDts >= 0)
        {
            // Calcular incremento mínimo baseado no time_base do stream
            AVRational streamTimeBase = audioStream->time_base;
            int64_t minDtsIncrement = av_rescale_q(m_ffmpeg.audioFrameSize, {1, static_cast<int>(m_sampleRate)}, streamTimeBase);
            if (minDtsIncrement <= 0)
            {
                minDtsIncrement = 1; // Garantir pelo menos 1
            }

            // CRÍTICO: Garantir que DTS seja sempre MAIOR que o anterior (não igual)
            // O erro "non monotonically increasing" ocorre quando DTS é igual ao anterior
            if (pkt->dts <= m_ffmpeg.lastAudioDts)
            {
                pkt->dts = m_ffmpeg.lastAudioDts + minDtsIncrement;
            }

            // Se DTS foi ajustado, garantir que PTS >= DTS
            if (pkt->pts < pkt->dts)
            {
                pkt->pts = pkt->dts;
            }
        }
        // Atualizar lastAudioDts APÓS todas as verificações
        m_ffmpeg.lastAudioDts = pkt->dts;

        muxPacket(pkt);

        av_packet_unref(pkt);
        encoded = true;
    }

    av_packet_free(&pkt);
    return encoded;
}

bool HTTPTSStreamer::muxPacket(void *packet)
{
    if (!packet || !m_active)
    {
        return false;
    }

    AVPacket *pkt = static_cast<AVPacket *>(packet);
    AVFormatContext *formatCtx = static_cast<AVFormatContext *>(m_ffmpeg.formatCtx);

    if (!formatCtx || !pkt)
    {
        return false;
    }

    // Usar av_interleaved_write_frame para garantir intercalação correta de áudio/vídeo
    // Isso é essencial para MPEG-TS - sem intercalação, o player pode descartar pacotes
    int ret = av_interleaved_write_frame(formatCtx, pkt);
    if (ret < 0)
    {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Falha ao escrever pacote MPEG-TS: " + std::string(errbuf));
        return false;
    }

    // Flush imediato após cada pacote para garantir envio contínuo
    // Isso é crítico para reduzir buffering no cliente
    AVIOContext *pb = formatCtx->pb;
    if (pb)
    {
        avio_flush(pb);
    }

    return true;
}

int HTTPTSStreamer::writeToClients(const uint8_t *buf, int buf_size)
{
    if (!buf || buf_size <= 0)
    {
        return -1;
    }

    // Copiar lista de sockets para evitar manter o lock por muito tempo
    std::vector<int> clientSocketsCopy;
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        clientSocketsCopy = m_clientSockets;
    }

    // Enviar para todos os clientes sem manter o lock
    // Isso evita bloquear o callback do FFmpeg por muito tempo
    std::vector<int> disconnectedClients;

    for (int clientFd : clientSocketsCopy)
    {
        // Enviar todos os dados, garantindo que tudo seja enviado
        const uint8_t *data = buf;
        size_t remaining = buf_size;

        while (remaining > 0)
        {
            ssize_t sent = send(clientFd, data, remaining, MSG_NOSIGNAL);

            if (sent < 0)
            {
                // Erro ao enviar - cliente desconectado
                disconnectedClients.push_back(clientFd);
                break;
            }

            data += sent;
            remaining -= sent;
        }
    }

    // Remover clientes desconectados (com lock)
    if (!disconnectedClients.empty())
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        for (int clientFd : disconnectedClients)
        {
            auto it = std::find(m_clientSockets.begin(), m_clientSockets.end(), clientFd);
            if (it != m_clientSockets.end())
            {
                close(clientFd);
                m_clientSockets.erase(it);
                uint32_t countAfter = --m_clientCount;
                LOG_INFO("Cliente HTTP MPEG-TS desconectado (erro ao enviar dados, total: " + std::to_string(countAfter) + ")");
            }
        }
    }

    return buf_size;
}
