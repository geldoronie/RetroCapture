#pragma once

#include "IVideoCapture.h"
#include <memory>

/**
 * @brief Factory for creating platform-specific video capture implementations
 */
class VideoCaptureFactory
{
public:
    /**
     * @brief Create a video capture instance for the current platform
     * @return Unique pointer to IVideoCapture implementation
     */
    static std::unique_ptr<IVideoCapture> create();
};
