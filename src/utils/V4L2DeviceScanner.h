#pragma once

#include <string>
#include <vector>

/**
 * Scans for V4L2 video capture devices.
 * 
 * This class extracts device scanning logic from UIManager
 * while preserving the exact same behavior.
 */
class V4L2DeviceScanner {
public:
    /**
     * Scan for V4L2 video capture devices.
     * 
     * @return Vector of device paths (e.g., "/dev/video0", "/dev/video1")
     */
    static std::vector<std::string> scan();
};

