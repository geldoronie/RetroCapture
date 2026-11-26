#pragma once

#include "../renderer/glad_loader.h"
#include "SourcePrePass.h"
#include "TemporalDenoiser.h"
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
    // When source adjustments are active (#228) this is the adjusted copy,
    // so the shader chain and every output see the corrected image.
    GLuint getTexture() const
    {
        if (m_denoisedTexture) return m_denoisedTexture;
        if (m_processedTexture) return m_processedTexture;
        return m_externalTexture ? m_externalTexture : m_texture;
    }

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

    /**
     * Source adjustments applied before the shader chain (#228). Takes
     * effect on the next processFrame(); call on the GL thread.
     */
    void setSourceAdjustments(const SourceAdjustments &adj);
    const SourceAdjustments &getSourceAdjustments() const { return m_adjust; }

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

    // libswscale YUYV->RGB, one context per horizontal slice: the slices are
    // converted in parallel (SWS_POINT and 4:2:2 have no vertical
    // dependency, so the result is bit-identical to one full-frame call).
    // A single-threaded 1080p conversion took ~9 ms per frame, over half of
    // a 60 fps frame budget. Recreated when the dimensions change.
    struct SwsSlice
    {
        SwsContext *ctx = nullptr;
        int y0 = 0;
        int h = 0;
    };
    std::vector<SwsSlice> m_swsSlices;
    int m_swsWidth = 0;
    int m_swsHeight = 0;
    void freeSwsSlices();

    // #228 — source colour adjustments and the GPU pass applying them.
    SourceAdjustments m_adjust;
    bool m_adjustDirty = false;
    SourcePrePass m_prePass;
    GLuint m_processedTexture = 0; // m_prePass output, 0 = pass bypassed
    TemporalDenoiser m_denoiser;   // #268
    GLuint m_denoisedTexture = 0;  // m_denoiser output, 0 = off
    bool m_lastFrameWasYUV = false;
    int m_reportedColorRange = 0;  // capture's quantization hint
    bool m_swsFullRange = false;   // YUV range the sws context decodes

    // newFrame = false: the same frame again (adjustments changed while the
    // source is idle), which must not advance the denoiser's history.
    void runPrePass(bool newFrame = true);
    bool wantYUVFullRange() const;

    // #229 — deinterlacing. Bob shows the first field when the frame
    // arrives and the second one half a frame period later, as its own
    // frame (new sequence number, timestamp + half a period).
    int m_reportedFieldOrder = 0;   // capture's field-order hint
    int m_currentField = 0;         // field bob keeps (0 = top/even rows)
    bool m_secondFieldPending = false;
    int64_t m_secondFieldDueUs = 0; // MediaClock time to present it
    int64_t m_secondFieldTsUs = 0;  // its capture timestamp
    int64_t m_lastFrameTsUs = 0;
    int64_t m_framePeriodUs = 0;    // estimated source frame period
    uint64_t m_droppedSecondFields = 0;

    int firstField() const;
    void beginFrameFields(int64_t frameTsUs);
    bool emitSecondFieldIfDue();

    // Texture filtering configurável
    bool m_textureFilterLinear = false; // Padrão: GL_NEAREST (mais rápido)

    /**
     * Convert YUYV (V4L2_PIX_FMT_YUYV) to RGB24 using libswscale.
     * libswscale dispatches to SIMD paths internally (SSE2/AVX/NEON).
     */
    void convertYUYVtoRGB(const uint8_t* yuyv, uint8_t* rgb, uint32_t width, uint32_t height);
};

