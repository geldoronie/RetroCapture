#pragma once

#include "../renderer/glad_loader.h"
#include "../capture/VideoCapture.h"
#include <cstdint>

/**
 * Processes video frames from capture devices.
 * 
 * Responsibilities:
 * - Convert frame formats (YUYV to RGB)
 * - Create and update OpenGL textures
 * - Manage texture lifecycle
 * 
 * This class follows the Single Responsibility Principle by isolating
 * frame processing logic from application orchestration.
 */
class FrameProcessor {
public:
    FrameProcessor();
    ~FrameProcessor();
    
    /**
     * Process a frame and update/create the OpenGL texture.
     * @param frame Frame data from VideoCapture
     * @param existingTexture Existing texture ID (0 to create new)
     * @param textureWidth Current texture width (0 if new)
     * @param textureHeight Current texture height (0 if new)
     * @param renderer Renderer for format conversion (optional, can be nullptr)
     * @param[out] outputTexture Output texture ID
     * @param[out] outputWidth Output texture width
     * @param[out] outputHeight Output texture height
     * @return true if frame was processed successfully, false otherwise
     */
    bool processFrame(const Frame& frame, 
                      GLuint existingTexture,
                      uint32_t textureWidth,
                      uint32_t textureHeight,
                      class OpenGLRenderer* renderer,
                      GLuint& outputTexture,
                      uint32_t& outputWidth,
                      uint32_t& outputHeight);
    
    /**
     * Convert YUYV format to RGB.
     * @param yuyv Input YUYV data
     * @param rgb Output RGB buffer (must be width * height * 3 bytes)
     * @param width Frame width
     * @param height Frame height
     */
    static void convertYUYVtoRGB(const uint8_t* yuyv, uint8_t* rgb, uint32_t width, uint32_t height);
    
    /**
     * Clean up resources (delete texture if owned).
     * Note: The caller is responsible for texture lifecycle, but this can be used
     * to clean up if FrameProcessor creates textures.
     */
    void cleanup();

private:
    /**
     * Create a new OpenGL texture with the specified dimensions.
     * @param width Texture width
     * @param height Texture height
     * @return Texture ID, or 0 on failure
     */
    GLuint createTexture(uint32_t width, uint32_t height);
    
    /**
     * Update an existing texture with new frame data.
     * @param texture Texture ID
     * @param rgbData RGB data
     * @param width Frame width
     * @param height Frame height
     * @param isNewTexture true if this is the first upload (use glTexImage2D), false for updates (glTexSubImage2D)
     */
    void updateTexture(GLuint texture, const uint8_t* rgbData, uint32_t width, uint32_t height, bool isNewTexture);
};

