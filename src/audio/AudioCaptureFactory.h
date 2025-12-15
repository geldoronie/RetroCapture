#pragma once

#include "IAudioCapture.h"
#include <memory>

/**
 * @brief Factory for creating platform-specific audio capture implementations
 */
class AudioCaptureFactory
{
public:
    /**
     * @brief Create an audio capture instance for the current platform
     * @return Unique pointer to IAudioCapture implementation
     */
    static std::unique_ptr<IAudioCapture> create();
};
