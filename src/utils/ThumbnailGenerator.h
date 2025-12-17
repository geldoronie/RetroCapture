#pragma once

#include "../renderer/glad_loader.h"
#include <string>
#include <cstdint>

/**
 * @brief Generates thumbnails from OpenGL framebuffer or textures
 * 
 * Captures frames and saves them as PNG images for preset thumbnails.
 */
class ThumbnailGenerator
{
public:
    ThumbnailGenerator();
    ~ThumbnailGenerator() = default;

    /**
     * @brief Capture current framebuffer and save as PNG thumbnail
     * @param outputPath Full path to output PNG file
     * @param width Thumbnail width (default: 320)
     * @param height Thumbnail height (default: 240)
     * @return true if successful, false otherwise
     * 
     * Note: This captures from the default framebuffer (0) after rendering.
     * Make sure to call this after the frame has been rendered to screen.
     */
    bool captureAndSaveThumbnail(
        const std::string& outputPath,
        uint32_t width = 320,
        uint32_t height = 240
    );

    /**
     * @brief Capture a specific texture and save as PNG thumbnail
     * @param texture OpenGL texture ID to capture
     * @param textureWidth Original texture width
     * @param textureHeight Original texture height
     * @param outputPath Full path to output PNG file
     * @param thumbnailWidth Thumbnail width (default: 320)
     * @param thumbnailHeight Thumbnail height (default: 240)
     * @return true if successful, false otherwise
     * 
     * This method creates a temporary framebuffer to read from the texture.
     */
    bool captureTextureAsThumbnail(
        GLuint texture,
        uint32_t textureWidth,
        uint32_t textureHeight,
        const std::string& outputPath,
        uint32_t thumbnailWidth = 320,
        uint32_t thumbnailHeight = 240
    );

private:
    /**
     * @brief Save RGB data as PNG file
     * @param data RGB pixel data (width * height * 3 bytes)
     * @param width Image width
     * @param height Image height
     * @param outputPath Full path to output PNG file
     * @return true if successful, false otherwise
     */
    bool savePNG(
        const uint8_t* data,
        uint32_t width,
        uint32_t height,
        const std::string& outputPath
    );

    /**
     * @brief Resize RGB image data using simple nearest-neighbor scaling
     * @param inputData Source RGB data
     * @param inputWidth Source width
     * @param inputHeight Source height
     * @param outputData Destination RGB data (must be pre-allocated)
     * @param outputWidth Destination width
     * @param outputHeight Destination height
     */
    void resizeImage(
        const uint8_t* inputData,
        uint32_t inputWidth,
        uint32_t inputHeight,
        uint8_t* outputData,
        uint32_t outputWidth,
        uint32_t outputHeight
    );
};

