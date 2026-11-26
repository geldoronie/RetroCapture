#include "FrameProcessor.h"
#include "../utils/MediaClock.h"
#include "../capture/IVideoCapture.h"
#include "../renderer/OpenGLRenderer.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <iostream>
#include <thread>

// Core GL since 1.2; define defensively in case the active loader header
// didn't expose it (used by the screen-capture 32-bit upload path).
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifdef __linux__
#include <linux/videodev2.h>
#ifndef V4L2_PIX_FMT_MJPEG
#define V4L2_PIX_FMT_MJPEG v4l2_fourcc('M', 'J', 'P', 'G')
#endif
#endif

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

FrameProcessor::FrameProcessor()
{
}

FrameProcessor::~FrameProcessor()
{
    deleteTexture();
    m_prePass.destroy();
    m_denoiser.destroy();
    freeSwsSlices();
}

void FrameProcessor::freeSwsSlices()
{
    for (SwsSlice &s : m_swsSlices)
        if (s.ctx) sws_freeContext(s.ctx);
    m_swsSlices.clear();
}

void FrameProcessor::init(OpenGLRenderer *renderer)
{
    m_renderer = renderer;
}

bool FrameProcessor::processFrame(IVideoCapture *capture)
{
    if (!capture)
    {
        LOG_WARN("FrameProcessor: capture is null");
        return false;
    }

    // IMPORTANT: Check if device is open before attempting to capture
    // This prevents segmentation faults during reconfiguration
    if (!capture->isOpen() && !capture->isDummyMode())
    {
        return false; // Device is closed, cannot process frames
    }

    m_reportedColorRange = capture->getReportedColorRange();
    m_reportedFieldOrder = capture->getReportedFieldOrder();

    // #107 zero-copy GPU path: if the capture hands us a ready GL texture
    // (DMABUF imported via EGL), use it directly and skip the CPU upload.
    {
        uint32_t gw = 0, gh = 0;
        const unsigned int gpuTex = capture->getGpuTexture(gw, gh);
        if (gpuTex != 0 && gw > 0 && gh > 0)
        {
            m_externalTexture = gpuTex;
            m_textureWidth    = gw;
            m_textureHeight   = gh;
            m_hasValidFrame   = true;
            // The zero-copy path has no per-frame signal: date it now.
            ++m_frameSeq;
            m_frameTimestampUs = MediaClock::nowUs();
            m_lastFrameWasYUV = false;
            beginFrameFields(m_frameTimestampUs);
            runPrePass();
            return true;
        }
        m_externalTexture = 0; // not on the GPU path this frame
    }

    Frame frame;
    // Usar captureLatestFrame para descartar frames antigos e pegar apenas o mais recente
    bool captured = capture->captureLatestFrame(frame);
    
    // Log de depuração para dummy mode
    if (!captured)
    {
        // #229 — bob: present the second field of the last frame once its
        // time has come (half a frame period after the first).
        if (emitSecondFieldIfDue())
        {
            return true;
        }
        // #228 — adjustments changed while the source is idle (paused or
        // static screen): re-run the pass on the last uploaded frame so the
        // change shows without waiting for the next one.
        if (m_adjustDirty && m_hasValidFrame)
        {
            runPrePass(/*newFrame=*/false);
        }
        return false; // Nenhum frame novo disponível
    }

    // Validar dados do frame
    if (!frame.data || frame.size == 0 || frame.width == 0 || frame.height == 0)
    {
        LOG_WARN("Invalid frame received (data: " + std::string(frame.data ? "ok" : "null") +
                 ", size: " + std::to_string(frame.size) +
                 ", dim: " + std::to_string(frame.width) + "x" + std::to_string(frame.height) + ")");
        return false;
    }

    // #198 — a new captured frame: remember when it was captured.
    ++m_frameSeq;
    m_frameTimestampUs = frame.timestampUs > 0 ? frame.timestampUs : MediaClock::nowUs();
    beginFrameFields(m_frameTimestampUs);

    // Se a textura ainda não foi criada ou o tamanho mudou
    bool textureCreated = false;
    if (m_texture == 0 || m_textureWidth != frame.width || m_textureHeight != frame.height)
    {
        if (m_texture != 0)
        {
            glDeleteTextures(1, &m_texture);
        }

        m_textureWidth = frame.width;
        m_textureHeight = frame.height;

        // Criar textura vazia primeiro
        glGenTextures(1, &m_texture);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Texture filtering configurável (padrão: GL_NEAREST para melhor performance)
        GLenum filter = m_textureFilterLinear ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

        textureCreated = true;
        LOG_INFO("Textura criada: " + std::to_string(m_textureWidth) + "x" + std::to_string(m_textureHeight));
    }

    // Converter e atualizar textura
    glBindTexture(GL_TEXTURE_2D, m_texture);

    // Verificar formato do frame
    // YUYV: 2 bytes por pixel (formato comum V4L2)
    // RGB24: 3 bytes por pixel (formato comum DirectShow)
    bool isYUYV = false;
#ifdef __linux__
    // No Linux, verificar se é YUYV pelo formato ou tamanho
    // V4L2_PIX_FMT_YUYV e V4L2_PIX_FMT_MJPEG já estão definidos em <linux/videodev2.h>

    // Verificar se é MJPG (não suportado ainda)
    if (frame.format == V4L2_PIX_FMT_MJPEG)
    {
        LOG_ERROR("MJPG format detected but not supported. The device must be configured for YUYV.");
        return false;
    }

    if (frame.format == V4L2_PIX_FMT_YUYV || frame.size == frame.width * frame.height * 2)
    {
        isYUYV = true;
    }
#else
    // No Windows, verificar pelo tamanho (DirectShow geralmente usa RGB24)
    if (frame.size == frame.width * frame.height * 2)
    {
        isYUYV = true;
    }
#endif

    if (isYUYV)
    {
        // Validar tamanho do buffer YUYV
        size_t expectedSize = frame.width * frame.height * 2;
        if (frame.size < expectedSize)
        {
            LOG_ERROR("Tamanho do frame YUYV incorreto: esperado " + std::to_string(expectedSize) +
                      ", recebido " + std::to_string(frame.size));
            return false;
        }

        // Converter YUYV para RGB
        // Reutilizar buffer existente, redimensionar apenas se necessário
        size_t requiredSize = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 3;
        if (m_rgbBuffer.size() < requiredSize)
        {
            m_rgbBuffer.resize(requiredSize);
        }
        convertYUYVtoRGB(frame.data, m_rgbBuffer.data(), frame.width, frame.height);
        m_lastFrameWasYUV = true;

        if (textureCreated)
        {
            // Primeira vez: usar glTexImage2D
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame.width, frame.height, 0, GL_RGB, GL_UNSIGNED_BYTE, m_rgbBuffer.data());
        }
        else
        {
            // Atualização: usar glTexSubImage2D (mais rápido, não realoca)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height, GL_RGB, GL_UNSIGNED_BYTE, m_rgbBuffer.data());
        }
    }
    else if (frame.size == frame.width * frame.height * 3)
    {
        m_lastFrameWasYUV = false;
        // RGB24: usar diretamente
        if (textureCreated)
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame.width, frame.height, 0, GL_RGB, GL_UNSIGNED_BYTE, frame.data);
        }
        else
        {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height, GL_RGB, GL_UNSIGNED_BYTE, frame.data);
        }
    }
    else if (frame.format == RC_PIXFMT_BGRA || frame.format == RC_PIXFMT_RGBA ||
             frame.size == static_cast<size_t>(frame.width) * frame.height * 4)
    {
        // Packed 32-bit (screen capture, #107). Hand it to GL with the
        // matching source format and let the driver swizzle into the RGB
        // texture — no CPU colour conversion, which is what keeps a full
        // monitor / 4K grab at the compositor's frame rate.
        m_lastFrameWasYUV = false;
        const GLenum srcFmt = (frame.format == RC_PIXFMT_RGBA) ? GL_RGBA : GL_BGRA;
        if (textureCreated)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame.width, frame.height, 0,
                         srcFmt, GL_UNSIGNED_BYTE, frame.data);
        else
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
                            srcFmt, GL_UNSIGNED_BYTE, frame.data);
    }
    else
    {
        // Outros formatos: usar renderer (pode ter conversão específica)
        m_lastFrameWasYUV = false;
        if (textureCreated)
        {
            m_renderer->updateTexture(m_texture, frame.data, frame.width, frame.height, frame.format);
        }
        else
        {
            m_renderer->updateTexture(m_texture, frame.data, frame.width, frame.height, frame.format);
        }
    }

    m_hasValidFrame = true;
    runPrePass();
    return true; // Frame processado com sucesso
}

void FrameProcessor::setSourceAdjustments(const SourceAdjustments &adj)
{
    if (adj.colorRange == m_adjust.colorRange &&
        adj.saturation == m_adjust.saturation &&
        adj.gamma == m_adjust.gamma &&
        adj.deinterlace == m_adjust.deinterlace &&
        adj.fieldOrder == m_adjust.fieldOrder &&
        adj.expandAfterDecode == m_adjust.expandAfterDecode &&
        adj.temporalDenoise == m_adjust.temporalDenoise)
    {
        return;
    }
    if ((adj.temporalDenoise > 0.0f) != (m_adjust.temporalDenoise > 0.0f))
    {
        LOG_INFO(std::string("Temporal denoise: ") + (adj.temporalDenoise > 0.0f ? "on" : "off"));
    }
    if (adj.expandAfterDecode != m_adjust.expandAfterDecode)
    {
        LOG_INFO(std::string("Expand limited range after decode: ") +
                 (adj.expandAfterDecode ? "on" : "off"));
    }
    if (adj.deinterlace != m_adjust.deinterlace || adj.fieldOrder != m_adjust.fieldOrder)
    {
        static const char *kModes[] = {"off", "bob", "weave", "blend"};
        static const char *kOrders[] = {"auto", "tff", "bff"};
        const int m = static_cast<int>(adj.deinterlace);
        const int o = static_cast<int>(adj.fieldOrder);
        LOG_INFO(std::string("Deinterlace: ") + (m >= 0 && m <= 3 ? kModes[m] : "?") +
                 ", field order " + (o >= 0 && o <= 2 ? kOrders[o] : "?"));
        m_secondFieldPending = false;
    }
    m_adjust = adj;
    m_adjustDirty = true;
}

int FrameProcessor::firstField() const
{
    switch (m_adjust.fieldOrder)
    {
    case FieldOrder::TopFirst:
        return 0;
    case FieldOrder::BottomFirst:
        return 1;
    case FieldOrder::Auto:
    default:
        // Backend hint, else top field first (the common case).
        return m_reportedFieldOrder == 2 ? 1 : 0;
    }
}

void FrameProcessor::beginFrameFields(int64_t frameTsUs)
{
    // Source frame period from consecutive capture timestamps (EMA), used
    // to place bob's second field halfway to the next frame.
    if (m_lastFrameTsUs > 0)
    {
        const int64_t d = frameTsUs - m_lastFrameTsUs;
        if (d >= 5000 && d <= 200000)
            m_framePeriodUs = m_framePeriodUs > 0 ? (m_framePeriodUs * 7 + d) / 8 : d;
    }
    m_lastFrameTsUs = frameTsUs;

    if (m_secondFieldPending)
    {
        // The next frame arrived before the second field was shown (the
        // loop ran late): drop it rather than show it late.
        ++m_droppedSecondFields;
        if (m_droppedSecondFields == 1 || m_droppedSecondFields % 600 == 0)
            LOG_INFO("Deinterlace (bob): dropped second fields so far: " +
                     std::to_string(m_droppedSecondFields));
    }

    m_currentField = firstField();
    m_secondFieldPending = (m_adjust.deinterlace == DeinterlaceMode::Bob);
    if (m_secondFieldPending)
    {
        const int64_t half = (m_framePeriodUs > 0 ? m_framePeriodUs : 33367) / 2;
        // Present it from 3/4 of a field after the first one: a main loop
        // paced at 2x the source rate (e.g. 60 Hz for 30 fps) then always
        // has an iteration inside the window despite a few ms of jitter,
        // instead of missing it and dropping the field when the next frame
        // lands first. The timestamp stays at the exact half period.
        m_secondFieldDueUs = MediaClock::nowUs() + (half * 3) / 4;
        m_secondFieldTsUs = frameTsUs + half;
    }
}

bool FrameProcessor::emitSecondFieldIfDue()
{
    if (!m_secondFieldPending || !m_hasValidFrame ||
        m_adjust.deinterlace != DeinterlaceMode::Bob)
    {
        return false;
    }
    if (MediaClock::nowUs() < m_secondFieldDueUs)
    {
        return false;
    }
    m_secondFieldPending = false;
    m_currentField = 1 - firstField();
    ++m_frameSeq;
    m_frameTimestampUs = m_secondFieldTsUs;
    runPrePass();
    return true;
}

bool FrameProcessor::wantYUVFullRange() const
{
    switch (m_adjust.colorRange)
    {
    case SourceColorRange::Full:
        return true;
    case SourceColorRange::Limited:
        return false;
    case SourceColorRange::Auto:
    default:
        // Historical behaviour: Y'CbCr is limited range unless the driver
        // explicitly says otherwise.
        return m_reportedColorRange == 2;
    }
}

void FrameProcessor::runPrePass(bool newFrame)
{
    m_adjustDirty = false;

    SourcePrePass::Params p;
    p.saturation = m_adjust.saturation;
    p.gamma = m_adjust.gamma;
    // #229 — Weave (and Off) leave the woven frame as is.
    if (m_adjust.deinterlace == DeinterlaceMode::Bob)
    {
        p.deinterlace = 1;
        p.field = m_currentField;
    }
    else if (m_adjust.deinterlace == DeinterlaceMode::Blend)
    {
        p.deinterlace = 2;
    }
    // Y'CbCr frames get their range in the YUV->RGB decode (libswscale);
    // RGB frames are expanded here when the source is limited range.
    if (!m_lastFrameWasYUV)
    {
        p.expandLimitedRange =
            m_adjust.colorRange == SourceColorRange::Limited ||
            (m_adjust.colorRange == SourceColorRange::Auto && m_reportedColorRange == 1);
    }
    // #243 — independent of colorRange and of the frame format.
    p.expandAfterDecode = m_adjust.expandAfterDecode;

    const GLuint raw = m_externalTexture ? m_externalTexture : m_texture;
    if (!SourcePrePass::isActive(p))
        m_processedTexture = 0; // identity: hand out the uploaded texture
    else
        m_processedTexture = m_prePass.apply(raw, m_textureWidth, m_textureHeight, p);

    // #268 — temporal denoise on the adjusted picture, once per frame.
    if (m_adjust.temporalDenoise <= 0.0f)
    {
        m_denoisedTexture = 0;
        m_denoiser.reset();
        return;
    }
    if (!newFrame)
    {
        // The same frame re-run with new adjustments: restart the history
        // from it instead of averaging the frame with itself.
        m_denoiser.reset();
    }
    const GLuint src = m_processedTexture ? m_processedTexture : raw;
    m_denoisedTexture = m_denoiser.apply(src, m_textureWidth, m_textureHeight, m_adjust.temporalDenoise);
}

void FrameProcessor::deleteTexture()
{
    if (m_texture != 0)
    {
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
        m_textureWidth = 0;
        m_textureHeight = 0;
        m_hasValidFrame = false;
    }
    m_processedTexture = 0;
    m_denoisedTexture = 0;
    m_denoiser.reset();
    m_secondFieldPending = false;
    m_lastFrameTsUs = 0;
    m_framePeriodUs = 0;
}

void FrameProcessor::setTextureFilterLinear(bool linear)
{
    m_textureFilterLinear = linear;
    m_prePass.setFilterLinear(linear);
    m_denoiser.setFilterLinear(linear);
    // Atualizar textura existente se houver
    if (m_texture != 0)
    {
        glBindTexture(GL_TEXTURE_2D, m_texture);
        GLenum filter = linear ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    }
}

void FrameProcessor::convertYUYVtoRGB(const uint8_t *yuyv, uint8_t *rgb, uint32_t width, uint32_t height)
{
    if (!yuyv || !rgb)
    {
        LOG_ERROR("Invalid pointers in YUYV-to-RGB conversion");
        return;
    }

    if (m_swsSlices.empty() || m_swsWidth != static_cast<int>(width) || m_swsHeight != static_cast<int>(height))
    {
        freeSwsSlices();
        // Up to 4 slices (one per spare core), each at least 64 rows.
        const unsigned hw = std::thread::hardware_concurrency();
        int n = static_cast<int>(std::min<unsigned>(4u, hw > 1 ? hw - 1 : 1u));
        while (n > 1 && static_cast<int>(height) / n < 64) --n;
        const int base = static_cast<int>(height) / n, extra = static_cast<int>(height) % n;
        int y = 0;
        for (int i = 0; i < n; ++i)
        {
            SwsSlice sl;
            sl.y0 = y;
            sl.h = base + (i < extra ? 1 : 0);
            sl.ctx = sws_getContext(static_cast<int>(width), sl.h, AV_PIX_FMT_YUYV422,
                                    static_cast<int>(width), sl.h, AV_PIX_FMT_RGB24,
                                    SWS_POINT, nullptr, nullptr, nullptr);
            if (!sl.ctx)
            {
                LOG_ERROR("sws_getContext falhou para YUYV→RGB " +
                          std::to_string(width) + "x" + std::to_string(sl.h));
                freeSwsSlices();
                return;
            }
            m_swsSlices.push_back(sl);
            y += sl.h;
        }
        m_swsWidth = static_cast<int>(width);
        m_swsHeight = static_cast<int>(height);
        m_swsFullRange = false; // libswscale's default for YUYV input
        LOG_INFO("YUYV->RGB: " + std::to_string(width) + "x" + std::to_string(height) + " in " +
                 std::to_string(n) + " parallel slice(s)");
    }

    // #228 — source range of the Y'CbCr input. Only touch the contexts when
    // it differs from what they decode now, so the default stays bit-exact.
    const bool fullRange = wantYUVFullRange();
    if (fullRange != m_swsFullRange)
    {
        for (SwsSlice &sl : m_swsSlices)
        {
            int *invTable = nullptr, *table = nullptr;
            int srcRange = 0, dstRange = 0, brightness = 0, contrast = 0, saturation = 0;
            if (sws_getColorspaceDetails(sl.ctx, &invTable, &srcRange, &table, &dstRange,
                                         &brightness, &contrast, &saturation) >= 0)
            {
                sws_setColorspaceDetails(sl.ctx, invTable, fullRange ? 1 : 0, table, dstRange,
                                         brightness, contrast, saturation);
            }
        }
        LOG_INFO(std::string("YUV decode range: ") + (fullRange ? "full (0-255)" : "limited (16-235)"));
        m_swsFullRange = fullRange;
    }

    auto convertSlice = [&](const SwsSlice &sl) {
        const uint8_t *srcSlice[1] = {yuyv + static_cast<size_t>(sl.y0) * width * 2};
        int srcStride[1] = {static_cast<int>(width) * 2};
        uint8_t *dstSlice[1] = {rgb + static_cast<size_t>(sl.y0) * width * 3};
        int dstStride[1] = {static_cast<int>(width) * 3};
        sws_scale(sl.ctx, srcSlice, srcStride, 0, sl.h, dstSlice, dstStride);
    };
    // Slice 0 on this thread, the others on short-lived workers.
    std::vector<std::thread> workers;
    workers.reserve(m_swsSlices.size() > 1 ? m_swsSlices.size() - 1 : 0);
    for (size_t i = 1; i < m_swsSlices.size(); ++i)
        workers.emplace_back(convertSlice, std::cref(m_swsSlices[i]));
    convertSlice(m_swsSlices[0]);
    for (std::thread &t : workers)
        t.join();
}

