#include "ThumbnailGenerator.h"
#include "../utils/Logger.h"
#include "../utils/FilesystemCompat.h"
#include "../renderer/glad_loader.h"
#include <png.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

ThumbnailGenerator::ThumbnailGenerator()
{
}

bool ThumbnailGenerator::captureAndSaveThumbnail(
    const std::string& outputPath,
    uint32_t width,
    uint32_t height)
{
    // Get current viewport dimensions
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    uint32_t viewportWidth = static_cast<uint32_t>(viewport[2]);
    uint32_t viewportHeight = static_cast<uint32_t>(viewport[3]);
    uint32_t viewportX = static_cast<uint32_t>(viewport[0]);
    uint32_t viewportY = static_cast<uint32_t>(viewport[1]);

    if (viewportWidth == 0 || viewportHeight == 0)
    {
        LOG_ERROR("Invalid viewport dimensions for thumbnail capture");
        return false;
    }

    // Calculate aspect ratios
    float viewportAspect = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);
    float targetAspect = static_cast<float>(width) / static_cast<float>(height);

    // Calculate crop region for centered capture
    uint32_t cropWidth = viewportWidth;
    uint32_t cropHeight = viewportHeight;
    uint32_t cropX = 0;
    uint32_t cropY = 0;

    if (viewportAspect > targetAspect)
    {
        // Viewport is wider - crop horizontally (left/right)
        cropWidth = static_cast<uint32_t>(viewportHeight * targetAspect);
        cropX = (viewportWidth - cropWidth) / 2;
    }
    else
    {
        // Viewport is taller - crop vertically (top/bottom)
        cropHeight = static_cast<uint32_t>(viewportWidth / targetAspect);
        cropY = (viewportHeight - cropHeight) / 2;
    }

    // Read pixels from framebuffer (only the cropped region)
    std::vector<uint8_t> frameData(cropWidth * cropHeight * 3);
    
    // glReadPixels reads from bottom-left, so we need to adjust Y coordinate
    // Read with padding to handle alignment
    size_t readRowSizeUnpadded = static_cast<size_t>(cropWidth) * 3;
    size_t readRowSizePadded = ((readRowSizeUnpadded + 3) / 4) * 4;
    size_t totalSizeWithPadding = readRowSizePadded * static_cast<size_t>(cropHeight);
    
    std::vector<uint8_t> frameDataWithPadding(totalSizeWithPadding);
    
    // Read from the cropped region
    // glReadPixels Y coordinate is from bottom-left of viewport
    // cropY is from top of viewport, so we need to convert to bottom-left coordinate
    // The bottom of the crop region is at: viewportY + (viewportHeight - cropY - cropHeight)
    uint32_t readY = viewportY + (viewportHeight - cropY - cropHeight);
    
    glReadPixels(static_cast<GLint>(viewportX + cropX), static_cast<GLint>(readY),
                 static_cast<GLsizei>(cropWidth), static_cast<GLsizei>(cropHeight),
                 GL_RGB, GL_UNSIGNED_BYTE, frameDataWithPadding.data());

    // Remove padding and flip vertically (glReadPixels returns bottom-to-top, we need top-to-bottom)
    for (uint32_t row = 0; row < cropHeight; row++)
    {
        // glReadPixels returns rows from bottom to top, so we need to reverse
        uint32_t srcRow = cropHeight - 1 - row; // Read from bottom row first
        uint32_t dstRow = row; // Write to top row first

        const uint8_t* srcPtr = frameDataWithPadding.data() + (srcRow * readRowSizePadded);
        uint8_t* dstPtr = frameData.data() + (dstRow * readRowSizeUnpadded);
        memcpy(dstPtr, srcPtr, readRowSizeUnpadded);
    }

    // Resize to target dimensions
    std::vector<uint8_t> thumbnailData(width * height * 3);
    resizeImage(frameData.data(), cropWidth, cropHeight,
                thumbnailData.data(), width, height);

    // Save as PNG
    return savePNG(thumbnailData.data(), width, height, outputPath);
}

bool ThumbnailGenerator::captureTextureAsThumbnail(
    GLuint texture,
    uint32_t textureWidth,
    uint32_t textureHeight,
    const std::string& outputPath,
    uint32_t thumbnailWidth,
    uint32_t thumbnailHeight)
{
    if (texture == 0 || textureWidth == 0 || textureHeight == 0)
    {
        LOG_ERROR("Invalid texture parameters for thumbnail capture");
        return false;
    }

    // Create temporary framebuffer
    GLuint framebuffer = 0;
    GLuint previousFramebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, reinterpret_cast<GLint*>(&previousFramebuffer));

    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        LOG_ERROR("Failed to create framebuffer for texture capture");
        glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
        glDeleteFramebuffers(1, &framebuffer);
        return false;
    }

    // Read pixels from texture
    std::vector<uint8_t> frameData(textureWidth * textureHeight * 3);
    
    size_t readRowSizeUnpadded = static_cast<size_t>(textureWidth) * 3;
    size_t readRowSizePadded = ((readRowSizeUnpadded + 3) / 4) * 4;
    size_t totalSizeWithPadding = readRowSizePadded * static_cast<size_t>(textureHeight);
    
    std::vector<uint8_t> frameDataWithPadding(totalSizeWithPadding);
    
    glReadPixels(0, 0, static_cast<GLsizei>(textureWidth), static_cast<GLsizei>(textureHeight),
                 GL_RGB, GL_UNSIGNED_BYTE, frameDataWithPadding.data());

    // Remove padding and flip vertically
    for (uint32_t row = 0; row < textureHeight; row++)
    {
        uint32_t srcRow = textureHeight - 1 - row; // Vertical flip
        uint32_t dstRow = row;

        const uint8_t* srcPtr = frameDataWithPadding.data() + (srcRow * readRowSizePadded);
        uint8_t* dstPtr = frameData.data() + (dstRow * readRowSizeUnpadded);
        memcpy(dstPtr, srcPtr, readRowSizeUnpadded);
    }

    // Restore previous framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, previousFramebuffer);
    glDeleteFramebuffers(1, &framebuffer);

    // Resize if needed
    std::vector<uint8_t> thumbnailData;
    const uint8_t* dataToSave = frameData.data();
    uint32_t saveWidth = textureWidth;
    uint32_t saveHeight = textureHeight;

    if (textureWidth != thumbnailWidth || textureHeight != thumbnailHeight)
    {
        thumbnailData.resize(thumbnailWidth * thumbnailHeight * 3);
        resizeImage(frameData.data(), textureWidth, textureHeight,
                    thumbnailData.data(), thumbnailWidth, thumbnailHeight);
        dataToSave = thumbnailData.data();
        saveWidth = thumbnailWidth;
        saveHeight = thumbnailHeight;
    }

    // Save as PNG
    return savePNG(dataToSave, saveWidth, saveHeight, outputPath);
}

void ThumbnailGenerator::resizeImage(
    const uint8_t* inputData,
    uint32_t inputWidth,
    uint32_t inputHeight,
    uint8_t* outputData,
    uint32_t outputWidth,
    uint32_t outputHeight)
{
    // Simple nearest-neighbor scaling
    for (uint32_t y = 0; y < outputHeight; y++)
    {
        for (uint32_t x = 0; x < outputWidth; x++)
        {
            // Calculate source coordinates
            uint32_t srcX = (x * inputWidth) / outputWidth;
            uint32_t srcY = (y * inputHeight) / outputHeight;

            // Clamp to valid range
            srcX = std::min(srcX, inputWidth - 1);
            srcY = std::min(srcY, inputHeight - 1);

            // Copy pixel (RGB = 3 bytes)
            size_t srcIndex = (srcY * inputWidth + srcX) * 3;
            size_t dstIndex = (y * outputWidth + x) * 3;

            outputData[dstIndex + 0] = inputData[srcIndex + 0]; // R
            outputData[dstIndex + 1] = inputData[srcIndex + 1]; // G
            outputData[dstIndex + 2] = inputData[srcIndex + 2]; // B
        }
    }
}

bool ThumbnailGenerator::savePNG(
    const uint8_t* data,
    uint32_t width,
    uint32_t height,
    const std::string& outputPath)
{
    if (!data || width == 0 || height == 0)
    {
        LOG_ERROR("Invalid parameters for PNG save");
        return false;
    }

    // Ensure output directory exists
    fs::path path(outputPath);
    fs::path dir = path.parent_path();
    if (!dir.empty() && !fs::exists(dir))
    {
        try
        {
            fs::create_directories(dir);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("Failed to create thumbnail directory: " + std::string(e.what()));
            return false;
        }
    }

    FILE* fp = fopen(outputPath.c_str(), "wb");
    if (!fp)
    {
        LOG_ERROR("Failed to open file for writing: " + outputPath);
        return false;
    }

    // Create PNG write structures
    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr)
    {
        fclose(fp);
        LOG_ERROR("Failed to create PNG write structure");
        return false;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr)
    {
        png_destroy_write_struct(&png_ptr, nullptr);
        fclose(fp);
        LOG_ERROR("Failed to create PNG info structure");
        return false;
    }

    // Set error handling
    if (setjmp(png_jmpbuf(png_ptr)))
    {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        LOG_ERROR("Error during PNG write");
        return false;
    }

    // Set I/O
    png_init_io(png_ptr, fp);

    // Set PNG header
    png_set_IHDR(
        png_ptr,
        info_ptr,
        width,
        height,
        8, // bit depth
        PNG_COLOR_TYPE_RGB,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT
    );

    // Write header
    png_write_info(png_ptr, info_ptr);

    // Prepare row pointers (libpng expects top-to-bottom, but we have bottom-to-top)
    // So we need to write rows in reverse order
    std::vector<png_bytep> row_pointers(height);
    size_t rowbytes = width * 3;

    for (uint32_t y = 0; y < height; y++)
    {
        // Write from bottom to top (flip vertically)
        uint32_t srcRow = height - 1 - y;
        row_pointers[y] = const_cast<png_byte*>(data + (srcRow * rowbytes));
    }

    // Write image data
    png_write_image(png_ptr, row_pointers.data());

    // Write end
    png_write_end(png_ptr, info_ptr);

    // Cleanup
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(fp);

    LOG_INFO("Thumbnail saved: " + outputPath);
    return true;
}

