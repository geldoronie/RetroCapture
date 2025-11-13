#pragma once

#include "../renderer/glad_loader.h"
#include <cstdint>
#include <vector>

// Forward declarations
struct Frame;
class VideoCapture;
class OpenGLRenderer;

/**
 * Processes video frames from V4L2 capture and converts them to OpenGL textures.
 * 
 * This class extracts frame processing logic from Application
 * while preserving the exact same behavior.
 */
class FrameProcessor {
public:
    FrameProcessor();
    ~FrameProcessor();

    /**
     * Initialize the frame processor.
     * 
     * @param renderer OpenGL renderer (for texture updates)
     */
    void init(OpenGLRenderer* renderer);

    /**
     * Process a frame from the capture device.
     * 
     * @param capture Video capture device
     * @return true if a new frame was processed, false otherwise
     */
    bool processFrame(VideoCapture* capture);

    /**
     * Get the current texture ID.
     * 
     * @return OpenGL texture ID, or 0 if no texture exists
     */
    GLuint getTexture() const { return m_texture; }

    /**
     * Get the texture width.
     * 
     * @return Texture width in pixels
     */
    uint32_t getTextureWidth() const { return m_textureWidth; }

    /**
     * Get the texture height.
     * 
     * @return Texture height in pixels
     */
    uint32_t getTextureHeight() const { return m_textureHeight; }

    /**
     * Check if a valid frame has been processed.
     * 
     * @return true if a valid frame exists, false otherwise
     */
    bool hasValidFrame() const { return m_hasValidFrame; }

    /**
     * Delete the current texture (call when reconfiguring).
     */
    void deleteTexture();

private:
    OpenGLRenderer* m_renderer = nullptr;
    GLuint m_texture = 0;
    uint32_t m_textureWidth = 0;
    uint32_t m_textureHeight = 0;
    bool m_hasValidFrame = false;

    /**
     * Convert YUYV format to RGB.
     * 
     * @param yuyv Input YUYV data
     * @param rgb Output RGB buffer (must be width * height * 3 bytes)
     * @param width Frame width
     * @param height Frame height
     */
    void convertYUYVtoRGB(const uint8_t* yuyv, uint8_t* rgb, uint32_t width, uint32_t height);
};

