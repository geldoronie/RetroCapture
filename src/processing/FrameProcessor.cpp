#include "FrameProcessor.h"
#include "../capture/IVideoCapture.h"
#include "../renderer/OpenGLRenderer.h"
#include "../utils/Logger.h"
#include <iostream>
#ifdef __linux__
#include <linux/videodev2.h>
#ifndef V4L2_PIX_FMT_MJPEG
#define V4L2_PIX_FMT_MJPEG v4l2_fourcc('M', 'J', 'P', 'G')
#endif
#endif

FrameProcessor::FrameProcessor()
{
}

FrameProcessor::~FrameProcessor()
{
    deleteTexture();
}

void FrameProcessor::init(OpenGLRenderer *renderer)
{
    m_renderer = renderer;
}

bool FrameProcessor::processFrame(IVideoCapture *capture)
{
    if (!capture)
    {
        LOG_WARN("FrameProcessor: capture é nullptr");
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
    if (capture->isDummyMode())
    {
        static int logCount = 0;
        if (logCount++ < 10) // Log as primeiras 10 vezes
        {
            LOG_INFO("FrameProcessor: captureLatestFrame retornou " + std::string(captured ? "true" : "false") + 
                     " no dummy mode (tentativa " + std::to_string(logCount) + ")");
            if (captured)
            {
                LOG_INFO("FrameProcessor: Frame recebido - data: " + std::string(frame.data ? "ok" : "null") +
                         ", size: " + std::to_string(frame.size) +
                         ", dim: " + std::to_string(frame.width) + "x" + std::to_string(frame.height));
            }
            else
            {
                LOG_WARN("FrameProcessor: captureLatestFrame retornou false - verificando motivo...");
            }
        }
    }
    
    if (!captured)
    {
        return false; // Nenhum frame novo disponível
    }

    // Validar dados do frame
    if (!frame.data || frame.size == 0 || frame.width == 0 || frame.height == 0)
    {
        LOG_WARN("Frame inválido recebido (data: " + std::string(frame.data ? "ok" : "null") +
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
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

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
        LOG_ERROR("Formato MJPG detectado mas não suportado. O dispositivo deve ser configurado para YUYV.");
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
        std::vector<uint8_t> rgbBuffer(frame.width * frame.height * 3);
        convertYUYVtoRGB(frame.data, rgbBuffer.data(), frame.width, frame.height);

        if (textureCreated)
        {
            // Primeira vez: usar glTexImage2D
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, frame.width, frame.height, 0, GL_RGB, GL_UNSIGNED_BYTE, rgbBuffer.data());
        }
        else
        {
            // Atualização: usar glTexSubImage2D (mais rápido, não realoca)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height, GL_RGB, GL_UNSIGNED_BYTE, rgbBuffer.data());
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

void FrameProcessor::convertYUYVtoRGB(const uint8_t *yuyv, uint8_t *rgb, uint32_t width, uint32_t height)
{
    // Validar ponteiros
    if (!yuyv || !rgb)
    {
        LOG_ERROR("Ponteiros inválidos na conversão YUYV para RGB");
        return;
    }

    // Conversão YUYV para RGB
    // YUYV: Y0 U0 Y1 V0 Y2 U1 Y3 V1 ...
    // Layout: para cada par de pixels, temos Y0 U Y1 V
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; x += 2)
        {
            // Índice no buffer YUYV (2 bytes por pixel, mas U e V são compartilhados)
            // YUYV layout: Y0 U Y1 V Y2 U Y3 V ...
            // Para cada par de pixels (x, x+1), temos 4 bytes: Y0 U Y1 V
            uint32_t idx = (y * width + x) * 2;

            // Verificar limites do buffer - garantir que temos pelo menos 4 bytes
            if (x + 1 >= width || idx + 3 >= width * height * 2)
            {
                // Se não temos um par completo de pixels, pular este
                continue;
            }

            int y0 = yuyv[idx];
            int u = yuyv[idx + 1];
            int y1 = yuyv[idx + 2];
            int v = yuyv[idx + 3];

            // Converter primeiro pixel
            int c = y0 - 16;
            int d = u - 128;
            int e = v - 128;

            int r0 = (298 * c + 409 * e + 128) >> 8;
            int g0 = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b0 = (298 * c + 516 * d + 128) >> 8;

            r0 = (r0 < 0) ? 0 : (r0 > 255) ? 255
                                           : r0;
            g0 = (g0 < 0) ? 0 : (g0 > 255) ? 255
                                           : g0;
            b0 = (b0 < 0) ? 0 : (b0 > 255) ? 255
                                           : b0;

            // Converter segundo pixel
            c = y1 - 16;
            int r1 = (298 * c + 409 * e + 128) >> 8;
            int g1 = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b1 = (298 * c + 516 * d + 128) >> 8;

            r1 = (r1 < 0) ? 0 : (r1 > 255) ? 255
                                           : r1;
            g1 = (g1 < 0) ? 0 : (g1 > 255) ? 255
                                           : g1;
            b1 = (b1 < 0) ? 0 : (b1 > 255) ? 255
                                           : b1;

            // Escrever pixels RGB
            uint32_t rgbIdx0 = (y * width + x) * 3;
            rgb[rgbIdx0] = r0;
            rgb[rgbIdx0 + 1] = g0;
            rgb[rgbIdx0 + 2] = b0;

            uint32_t rgbIdx1 = (y * width + x + 1) * 3;
            rgb[rgbIdx1] = r1;
            rgb[rgbIdx1 + 1] = g1;
            rgb[rgbIdx1 + 2] = b1;
        }
    }
}
