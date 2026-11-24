#pragma once

#include "../renderer/glad_loader.h"
#include <cstdint>
#include <vector>

// Forward declarations
struct Frame;
class IVideoCapture;
class OpenGLRenderer;
struct SwsContext;

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
    bool processFrame(IVideoCapture* capture);

    /**
     * Get the current texture ID.
     * 
     * @return OpenGL texture ID, or 0 if no texture exists
     */
    // Returns the external (zero-copy DMABUF) texture when the capture
    // provided one this frame, else the internally-uploaded texture.
    GLuint getTexture() const { return m_externalTexture ? m_externalTexture : m_texture; }

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

    // #198 — identity and capture time of the frame behind the current
    // texture. The sequence number changes once per captured frame (not
    // per render), so outputs can push each captured frame exactly once;
    // the timestamp is the backend's capture time on MediaClock (dequeue
    // time when the backend doesn't stamp frames).
    uint64_t getFrameSeq() const { return m_frameSeq; }
    int64_t getFrameTimestampUs() const { return m_frameTimestampUs; }

    /**
     * Delete the current texture (call when reconfiguring).
     */
    void deleteTexture();
    
    /**
     * Set texture filtering mode.
     *
     * @param linear true for GL_LINEAR, false for GL_NEAREST
     */
    void setTextureFilterLinear(bool linear);

    /**
     * Get current texture filtering mode.
     */
    bool getTextureFilterLinear() const { return m_textureFilterLinear; }

private:
    OpenGLRenderer* m_renderer = nullptr;
    GLuint m_texture = 0;
    // Externally-owned texture (capture's DMABUF zero-copy path, #107).
    // When non-zero, getTexture() returns it and we never delete it.
    GLuint m_externalTexture = 0;
    uint32_t m_textureWidth = 0;
    uint32_t m_textureHeight = 0;
    bool m_hasValidFrame = false;
    uint64_t m_frameSeq = 0;
    int64_t m_frameTimestampUs = 0;
    
    // Buffer RGB reutilizável para conversão YUYV→RGB
    // Redimensionado apenas quando necessário (quando dimensões mudam)
    std::vector<uint8_t> m_rgbBuffer;

    // Contexto libswscale para YUYV→RGB. Recriado se as dimensões mudarem.
    SwsContext* m_swsContext = nullptr;
    int m_swsWidth = 0;
    int m_swsHeight = 0;

    // Texture filtering configurável
    bool m_textureFilterLinear = false; // Padrão: GL_NEAREST (mais rápido)

    /**
     * Convert YUYV (V4L2_PIX_FMT_YUYV) to RGB24 using libswscale.
     * libswscale dispatches to SIMD paths internally (SSE2/AVX/NEON).
     */
    void convertYUYVtoRGB(const uint8_t* yuyv, uint8_t* rgb, uint32_t width, uint32_t height);
};

