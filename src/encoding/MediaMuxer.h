#pragma once

#include "MediaEncoder.h"
#include <cstdint>
#include <vector>
#include <functional>
#include <mutex>

/**
 * MediaMuxer - Classe responsável por muxing de pacotes codificados em container MPEG-TS
 *
 * Recebe pacotes codificados do MediaEncoder e os muxa em MPEG-TS,
 * enviando os dados através de um callback customizado.
 */
class MediaMuxer
{
public:
    // Callback para escrever dados muxados
    // Retorna número de bytes escritos, ou -1 em caso de erro
    using WriteCallback = std::function<int(const uint8_t *data, size_t size)>;

    MediaMuxer();
    ~MediaMuxer();

    // Inicializar muxer com configurações, codec contexts e callback
    // Os codec contexts são necessários para configurar os streams corretamente
    bool initialize(const MediaEncoder::VideoConfig &videoConfig,
                    const MediaEncoder::AudioConfig &audioConfig,
                    void *videoCodecContext, // AVCodecContext* do MediaEncoder
                    void *audioCodecContext, // AVCodecContext* do MediaEncoder
                    WriteCallback writeCallback);

    // Muxar um pacote codificado
    bool muxPacket(const MediaEncoder::EncodedPacket &packet);

    // Flush muxer - processar pacotes pendentes
    void flush();

    // Limpar recursos
    void cleanup();

    // Verificar se está inicializado
    bool isInitialized() const { return m_initialized; }

    // Obter header do formato (para enviar a novos clientes)
    std::vector<uint8_t> getFormatHeader() const;

    // Verificar se header foi escrito
    bool isHeaderWritten() const { return m_headerWritten; }

    // Método público para captura de header (chamado pelo callback)
    void captureFormatHeader(const uint8_t *buf, size_t buf_size);

    // Método público para chamar o callback de escrita (chamado pelo callback estático)
    int callWriteCallback(const uint8_t *buf, size_t buf_size);

private:
    // Inicializar streams no muxer
    bool initializeStreams(void *videoCodecContext, void *audioCodecContext);

    // Converter PTS/DTS do time_base do codec para time_base do stream
    void convertPTS(const MediaEncoder::EncodedPacket &packet, int64_t &pts, int64_t &dts);

    // Garantir monotonicidade de PTS/DTS no muxer
    void ensureMonotonicPTS(int64_t &pts, int64_t &dts, bool isVideo);

    // Configurações
    MediaEncoder::VideoConfig m_videoConfig;
    MediaEncoder::AudioConfig m_audioConfig;
    WriteCallback m_writeCallback;
    bool m_initialized = false;

    // Muxer context (void* para evitar incluir headers FFmpeg no .h)
    void *m_muxerContext = nullptr; // AVFormatContext*
    void *m_videoStream = nullptr;  // AVStream*
    void *m_audioStream = nullptr;  // AVStream*

    // Codec contexts (necessários para conversão de PTS/DTS)
    void *m_videoCodecContext = nullptr; // AVCodecContext*
    void *m_audioCodecContext = nullptr; // AVCodecContext*

    // Header do formato (capturado após primeira escrita)
    mutable std::mutex m_headerMutex;
    std::vector<uint8_t> m_formatHeader;
    bool m_headerWritten = false;
    size_t m_headerCaptureSize = 64 * 1024; // Capturar primeiros 64KB

    // Rastreamento de PTS/DTS para garantir monotonicidade
    std::mutex m_ptsMutex;
    int64_t m_lastVideoPTS = -1;
    int64_t m_lastVideoDTS = -1;
    int64_t m_lastAudioPTS = -1;
    int64_t m_lastAudioDTS = -1;

    // Mutex para proteger av_interleaved_write_frame (não é thread-safe)
    mutable std::mutex m_muxMutex;
};
