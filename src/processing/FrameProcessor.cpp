#include "FrameProcessor.h"
#include "../renderer/OpenGLRenderer.h"
#include "../utils/Logger.h"
#include <linux/videodev2.h>
#include <vector>

FrameProcessor::FrameProcessor()
{
}

FrameProcessor::~FrameProcessor()
{
    cleanup();
}

bool FrameProcessor::processFrame(const Frame& frame, 
                                  GLuint existingTexture,
                                  uint32_t textureWidth,
                                  uint32_t textureHeight,
                                  OpenGLRenderer* renderer,
                                  GLuint& outputTexture,
                                  uint32_t& outputWidth,
                                  uint32_t& outputHeight)
{
    if (!frame.data || frame.width == 0 || frame.height == 0)
    {
        return false;
    }

    // Check if texture needs to be created or resized
    bool textureCreated = false;
    GLuint texture = existingTexture;
    
    if (texture == 0 || textureWidth != frame.width || textureHeight != frame.height)
    {
        if (texture != 0)
        {
            glDeleteTextures(1, &texture);
        }

        texture = createTexture(frame.width, frame.height);
        if (texture == 0)
        {
            return false;
        }

        textureWidth = frame.width;
        textureHeight = frame.height;
        textureCreated = true;
        LOG_INFO("Textura criada: " + std::to_string(textureWidth) + "x" + std::to_string(textureHeight));
    }

    // Process frame based on format
    if (frame.format == V4L2_PIX_FMT_YUYV)
    {
        // Convert YUYV to RGB
        std::vector<uint8_t> rgbBuffer(frame.width * frame.height * 3);
        convertYUYVtoRGB(frame.data, rgbBuffer.data(), frame.width, frame.height);
        
        // Update texture
        updateTexture(texture, rgbBuffer.data(), frame.width, frame.height, textureCreated);
    }
    else
    {
        // Use renderer for other formats if available
        if (renderer)
        {
            renderer->updateTexture(texture, frame.data, frame.width, frame.height, frame.format);
        }
        else
        {
            LOG_WARN("Formato de frame não suportado e renderer não disponível");
            return false;
        }
    }

    // Return results
    outputTexture = texture;
    outputWidth = textureWidth;
    outputHeight = textureHeight;
    
    return true;
}

void FrameProcessor::convertYUYVtoRGB(const uint8_t* yuyv, uint8_t* rgb, uint32_t width, uint32_t height)
{
    // Conversão YUYV para RGB
    // YUYV: Y0 U0 Y1 V0 Y2 U1 Y3 V1 ...
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; x += 2)
        {
            uint32_t idx = y * width * 2 + x * 2;

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

            r0 = (r0 < 0) ? 0 : (r0 > 255) ? 255 : r0;
            g0 = (g0 < 0) ? 0 : (g0 > 255) ? 255 : g0;
            b0 = (b0 < 0) ? 0 : (b0 > 255) ? 255 : b0;

            uint32_t rgbIdx0 = (y * width + x) * 3;
            rgb[rgbIdx0] = r0;
            rgb[rgbIdx0 + 1] = g0;
            rgb[rgbIdx0 + 2] = b0;

            // Converter segundo pixel (mesmo U e V)
            c = y1 - 16;

            int r1 = (298 * c + 409 * e + 128) >> 8;
            int g1 = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b1 = (298 * c + 516 * d + 128) >> 8;

            r1 = (r1 < 0) ? 0 : (r1 > 255) ? 255 : r1;
            g1 = (g1 < 0) ? 0 : (g1 > 255) ? 255 : g1;
            b1 = (b1 < 0) ? 0 : (b1 > 255) ? 255 : b1;

            if (x + 1 < width)
            {
                uint32_t rgbIdx1 = (y * width + x + 1) * 3;
                rgb[rgbIdx1] = r1;
                rgb[rgbIdx1 + 1] = g1;
                rgb[rgbIdx1 + 2] = b1;
            }
        }
    }
}

GLuint FrameProcessor::createTexture(uint32_t width, uint32_t height)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (texture == 0)
    {
        return 0;
    }

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    return texture;
}

void FrameProcessor::updateTexture(GLuint texture, const uint8_t* rgbData, uint32_t width, uint32_t height, bool isNewTexture)
{
    glBindTexture(GL_TEXTURE_2D, texture);

    if (isNewTexture)
    {
        // First time: use glTexImage2D
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, rgbData);
    }
    else
    {
        // Update: use glTexSubImage2D (faster, doesn't reallocate)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgbData);
    }
}

void FrameProcessor::cleanup()
{
    // FrameProcessor doesn't own textures, so nothing to clean up here
    // This method exists for future extensibility
}

