#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <functional>

/**
 * MediaEncoder - Classe responsável por encoding de vídeo e áudio
 *
 * Recebe dados brutos (RGB para vídeo, int16 para áudio) e produz
 * pacotes codificados prontos para muxing.
 */
class MediaEncoder
{
public:
    // Hardware encoder backend selection. Auto picks the best available
    // at runtime (hardware > software); Software forces libx264. The
    // specific hardware backends are matched against what ffmpeg can
    // actually open on this host — detectAvailableEncoders() returns the
    // ones that opened cleanly at startup.
    enum class HardwareEncoder
    {
        Auto,      // detect — prefer hardware if available
        Software,  // libx264
        NVENC,     // h264_nvenc — NVIDIA dedicated encoder block
        VAAPI,     // h264_vaapi — Linux Intel/AMD via libva
        QSV,       // h264_qsv  — Intel Quick Sync Video
        AMF        // h264_amf  — AMD on Windows (Advanced Media Framework)
    };

    static const char *hardwareEncoderName(HardwareEncoder h);   // display label
    // Returns the ffmpeg codec name for the given backend, choosing
    // h264_* or hevc_* based on `isHEVC`. The other codecs in the
    // project (vp8/vp9) don't have hardware equivalents we support.
    static const char *hardwareEncoderCodec(HardwareEncoder h, bool isHEVC = false);
    // Returns the subset of HardwareEncoder values whose ffmpeg codec
    // can actually be opened on this machine. Always includes Software.
    // Probes both the H.264 and HEVC builds — a backend is reported as
    // available if either codec is compiled in. First call may probe;
    // subsequent calls return cached results.
    static std::vector<HardwareEncoder> detectAvailableEncoders();

    // Configuração de vídeo
    struct VideoConfig
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t fps = 60;
        uint32_t bitrate = 2000000; // 2 Mbps padrão
        std::string codec = "h264"; // "h264", "h265", "vp8", "vp9"
        std::string preset = "veryfast";
        std::string profile = "baseline"; // Para H.264
        std::string h265Profile = "main"; // Para H.265
        std::string h265Level = "auto";   // Para H.265
        int vp8Speed = 12;                // 0-16
        int vp9Speed = 6;                 // 0-9
        HardwareEncoder hardwareEncoder = HardwareEncoder::Auto;
        // Free-form quality / preset value whose interpretation depends
        // on the active hardware backend:
        //   NVENC: "p1".."p7"
        //   VAAPI: "CBR" / "VBR" / "CQP"
        //   QSV:   "veryfast".."veryslow"
        //   AMF:   "speed" / "balanced" / "quality"
        // Empty string falls back to the backend's hardcoded default.
        std::string hwPreset;
    };

    // Configuração de áudio
    struct AudioConfig
    {
        uint32_t sampleRate = 44100;
        uint32_t channels = 2;
        uint32_t bitrate = 128000; // 128 kbps padrão
        std::string codec = "aac";
    };

    // Pacote codificado (vídeo ou áudio)
    struct EncodedPacket
    {
        std::vector<uint8_t> data;
        int64_t pts = -1;               // Presentation Timestamp (-1 = AV_NOPTS_VALUE)
        int64_t dts = -1;               // Decode Timestamp (-1 = AV_NOPTS_VALUE)
        bool isKeyframe = false;        // Apenas para vídeo
        bool isVideo = true;            // true = vídeo, false = áudio
        int64_t captureTimestampUs = 0; // Timestamp original de captura
    };

    MediaEncoder();
    ~MediaEncoder();

    // Inicializar encoder com configurações
    // forStreaming: true para streaming (usa repeat-headers), false para gravação em arquivo (usa global header)
    bool initialize(const VideoConfig &videoConfig, const AudioConfig &audioConfig, bool forStreaming = false);

    // Encoding de vídeo: RGB → YUV → codec
    // Retorna true se frame foi enviado ao codec (pode gerar 0 ou mais pacotes)
    bool encodeVideo(const uint8_t *rgbData, uint32_t width, uint32_t height,
                     int64_t captureTimestampUs, std::vector<EncodedPacket> &packets);

    // Encoding de áudio: int16 → float planar → codec
    // Retorna true se samples foram processados (pode gerar 0 ou mais pacotes)
    // #199 — captureTimestampUs is the MediaClock capture time of
    // samples[0]; inRate / inChannels the capture format (resampled to the
    // encoder's format). Output audio PTS is sample-count based but steered
    // to stay on the capture clock (see encodeAudio()).
    bool encodeAudio(const int16_t *samples, size_t sampleCount,
                     int64_t captureTimestampUs, uint32_t inRate, uint32_t inChannels,
                     std::vector<EncodedPacket> &packets);

    // Flush codecs - processar frames pendentes
    void flush(std::vector<EncodedPacket> &packets);

    // Limpar recursos
    void cleanup();

    // Verificar se está inicializado
    bool isInitialized() const { return m_initialized; }

    // Obter configurações atuais
    const VideoConfig &getVideoConfig() const { return m_videoConfig; }
    const AudioConfig &getAudioConfig() const { return m_audioConfig; }

    // Obter codec contexts (para MediaMuxer configurar streams)
    void *getVideoCodecContext() const { return m_videoCodecContext; }
    void *getAudioCodecContext() const { return m_audioCodecContext; }

    // Contador de frames (para keyframes periódicos)
    int64_t getVideoFrameCount() const { return m_videoFrameCount; }
    void resetVideoFrameCount() { m_videoFrameCount = 0; }

    // Eventos de retrocesso de PTS (forçar pra frente para preservar monotonicidade).
    // Não-zero indica instabilidade no timestamp source.
    uint64_t getDesyncFrameCount() const { return m_desyncFrameCount.load(std::memory_order_relaxed); }

    // #123 — per-stage video encode timing, in microseconds, accumulated
    // since the last fetch. encodeVideo() splits its cost into:
    //   convertUs — convertRGBToYUV (CPU swscale: RGB→NV12 + any resize)
    //   uploadUs  — av_hwframe_transfer_data (sw NV12 → GPU surface; 0 for SW/NVENC)
    //   encodeUs  — avcodec_send_frame + receiveVideoPackets (codec)
    // frames is how many encodeVideo calls contributed. fetch resets the
    // accumulators so a caller can print a clean rolling average.
    struct VideoStageTimings { uint64_t convertUs = 0, uploadUs = 0, encodeUs = 0, frames = 0; };
    VideoStageTimings fetchVideoStageTimings()
    {
        VideoStageTimings t;
        t.convertUs = m_convertUs.exchange(0, std::memory_order_relaxed);
        t.uploadUs  = m_uploadUs.exchange(0, std::memory_order_relaxed);
        t.encodeUs  = m_encodeUs.exchange(0, std::memory_order_relaxed);
        t.frames    = m_stageFrames.exchange(0, std::memory_order_relaxed);
        return t;
    }

private:
    // Inicialização de codecs
    bool initializeVideoCodec();
    bool initializeAudioCodec();

    // Conversão de formatos
    bool convertRGBToYUV(const uint8_t *rgbData, uint32_t width, uint32_t height, void *videoFrame);
    bool createAudioResampler(uint32_t inRate, uint32_t inChannels);
    void writeSilenceToFifo(int64_t frames);
    bool drainAudioFifo(std::vector<EncodedPacket> &packets, int64_t captureTimestampUs, bool final);

    // Processar pacotes do codec
    bool receiveVideoPackets(std::vector<EncodedPacket> &packets, int64_t captureTimestampUs);
    bool receiveAudioPackets(std::vector<EncodedPacket> &packets, int64_t captureTimestampUs);

    // Calcular PTS baseado em timestamp de captura
    int64_t calculateVideoPTS(int64_t captureTimestampUs);

    // Garantir monotonicidade de PTS/DTS
    void ensureMonotonicPTS(int64_t &pts, int64_t &dts, bool isVideo);

    // Configurações
    VideoConfig m_videoConfig;
    AudioConfig m_audioConfig;
    bool m_initialized = false;
    bool m_forStreaming = false; // true para streaming, false para gravação em arquivo

    // Codec contexts (void* para evitar incluir headers FFmpeg no .h)
    void *m_videoCodecContext = nullptr; // AVCodecContext*
    void *m_audioCodecContext = nullptr; // AVCodecContext*

    // FFmpeg conversion contexts
    void *m_swsContext = nullptr; // SwsContext* (RGB to YUV)
    void *m_swrContext = nullptr; // SwrContext* (int16 to float planar)
    void *m_videoFrame = nullptr; // AVFrame* (para encoding de vídeo — software pixel buffer)
    void *m_hwVideoFrame = nullptr; // AVFrame* (hw surface destination, when using a HW encoder)
    void *m_audioFrame = nullptr; // AVFrame* (para encoding de áudio)

    // Hardware encoder runtime state. Empty when running software libx264.
    void *m_hwDeviceCtx = nullptr; // AVBufferRef* (AV_HWDEVICE_TYPE_*)
    void *m_hwFramesCtx = nullptr; // AVBufferRef* (AVHWFramesContext)
    HardwareEncoder m_activeHardwareEncoder = HardwareEncoder::Software;
    // Helpers used by initializeVideoCodec when a HW backend is selected.
    bool initializeHardwareVideoCodec(HardwareEncoder backend);
    bool createHardwareContext(HardwareEncoder backend);

    // Cache para dimensões do SwsContext
    uint32_t m_swsSrcWidth = 0;
    uint32_t m_swsSrcHeight = 0;
    uint32_t m_swsDstWidth = 0;
    uint32_t m_swsDstHeight = 0;
    int m_swsDstFormat = 0; // AVPixelFormat — invalidate sws ctx when destination format changes too

    // Padded scratch for the sws_scale source. libswscale's AVX2 RGB
    // fastpath reads a few SIMD chunks past the last row; if the
    // caller's buffer is allocated exactly width*height*3 and ends on
    // a page boundary that over-read segfaults. We copy the source in
    // here with trailing padding so the over-read lands in allocated
    // memory. See convertRGBToYUV.
    std::vector<uint8_t> m_swsSrcPadded;

    // #199 — shared A/V epoch: capture time of the first video frame.
    // Video PTS = capture time - epoch; audio output samples are placed on
    // the same timeline.
    int64_t m_epochUs = 0;
    bool    m_epochSet = false;

    // #199 — audio timeline. m_audioOutPos is the output-sample position
    // (since the epoch) of the next sample handed to the codec; the FIFO
    // holds resampled FLTP samples waiting to fill a codec frame.
    void *m_audioFifo = nullptr; // AVAudioFifo*
    uint32_t m_swrInRate = 0;
    uint32_t m_swrInChannels = 0;
    bool m_audioStarted = false;
    int64_t m_audioOutPos = 0;
    double m_audioDriftEma = 0.0; // smoothed (output timeline - capture time), in samples
    std::vector<std::vector<float>> m_audioPlanes; // resampler output scratch
    uint64_t m_audioGapsFilled = 0;
    uint64_t m_audioOverlapsDropped = 0;
    uint64_t m_videoFramesOutOfOrder = 0;
    // #241 — audio that reached encodeAudio() before the first video frame
    // latched the epoch. The callers drain audio before video in each
    // loop iteration, so at start the first iteration's audio (captured
    // AFTER the frame that becomes the epoch) arrived while there was no
    // epoch yet and used to be dropped: the output then began with up to
    // ~100 ms (more under load) of missing audio. It is held here (bounded
    // to the last second) and replayed by the next encodeAudio() call once
    // the epoch exists; the usual trim drops the part captured before it.
    struct PendingAudio
    {
        std::vector<int16_t> samples;
        int64_t captureTimestampUs = 0;
        uint32_t inRate = 0;
        uint32_t inChannels = 0;
    };
    std::deque<PendingAudio> m_preEpochAudio;
    bool encodeAudioChunk(const int16_t *samples, size_t sampleCount,
                          int64_t captureTimestampUs, uint32_t inRate, uint32_t inChannels,
                          std::vector<EncodedPacket> &packets);

    // #193 — MediaClock time of the last encodeAudio() call (0 = never).
    // With no audio for > 1 s, encodeVideo() keeps the audio track alive
    // with silence so the output always carries a valid audio stream.
    int64_t m_lastAudioInputUs = 0;
    bool m_silenceKeepaliveActive = false;
    void keepAudioAliveWithSilence(int64_t videoCaptureTsUs, std::vector<EncodedPacket> &packets);

    // Contadores precisos para cálculo de PTS (baseado em samples/frames, não timestamps)
    int64_t m_audioFrameCount = 0;  // Número de frames de áudio processados
    int64_t m_videoFrameCountForPTS = 0;  // Número de frames de vídeo processados (para PTS)

    // Rastreamento de PTS/DTS para garantir monotonicidade
    std::mutex m_ptsMutex;
    int64_t m_lastVideoPTS = -1;
    int64_t m_lastVideoDTS = -1;
    int64_t m_lastVideoFramePTS = -1; // PTS do frame antes de enviar ao codec
    int64_t m_lastAudioPTS = -1;
    int64_t m_lastAudioDTS = -1;
    int64_t m_lastAudioFramePTS = -1; // PTS do frame antes de enviar ao codec

    // Contador de frames para keyframes periódicos
    int64_t m_videoFrameCount = 0;

    // Contador de eventos de retrocesso de PTS: cada incremento é uma vez que
    // o calculatedPTS teria ficado <= ao último PTS já emitido e tivemos que
    // forçá-lo pra frente (m_lastXxxPTS + 1). Indica instabilidade no
    // timestamp source — o stream ainda fica monotônico, mas isso vira jitter
    // de duração de frame no arquivo final.
    std::atomic<uint64_t> m_desyncFrameCount{0};

    // #123 — per-stage encode timing accumulators (microseconds). See
    // fetchVideoStageTimings(). Populated by encodeVideo, read+reset by
    // the streaming telemetry. Per-instance, so /stream and /raw report
    // independently.
    std::atomic<uint64_t> m_convertUs{0};
    std::atomic<uint64_t> m_uploadUs{0};
    std::atomic<uint64_t> m_encodeUs{0};
    std::atomic<uint64_t> m_stageFrames{0};
};
