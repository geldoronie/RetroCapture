#include "FrameProcessor.h"
#include "../capture/IVideoCapture.h"
#include "../renderer/OpenGLRenderer.h"
#include "../utils/Logger.h"
#include <iostream>

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
    if (m_swsContext)
    {
        sws_freeContext(m_swsContext);
        m_swsContext = nullptr;
    }
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

    Frame frame;
    // Usar captureLatestFrame para descartar frames antigos e pegar apenas o mais recente
    bool captured = capture->captureLatestFrame(frame);
    
    // Log de depuração para dummy mode
    if (!captured)
    {
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
    return true; // Frame processado com sucesso
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
}

void FrameProcessor::setTextureFilterLinear(bool linear)
{
    m_textureFilterLinear = linear;
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

    if (!m_swsContext || m_swsWidth != static_cast<int>(width) || m_swsHeight != static_cast<int>(height))
    {
        if (m_swsContext)
        {
            sws_freeContext(m_swsContext);
            m_swsContext = nullptr;
        }
        m_swsContext = sws_getContext(
            static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_YUYV422,
            static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_RGB24,
            SWS_POINT, nullptr, nullptr, nullptr);
        if (!m_swsContext)
        {
            LOG_ERROR("sws_getContext falhou para YUYV→RGB " +
                      std::to_string(width) + "x" + std::to_string(height));
            return;
        }
        m_swsWidth = static_cast<int>(width);
        m_swsHeight = static_cast<int>(height);
    }

    const uint8_t *srcSlice[1] = { yuyv };
    int srcStride[1] = { static_cast<int>(width) * 2 };
    uint8_t *dstSlice[1] = { rgb };
    int dstStride[1] = { static_cast<int>(width) * 3 };

    sws_scale(m_swsContext, srcSlice, srcStride, 0, static_cast<int>(height), dstSlice, dstStride);
}

