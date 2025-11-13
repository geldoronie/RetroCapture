#pragma once

#include <string>
#include <vector>

/**
 * Scans system for available V4L2 video capture devices.
 * 
 * This class follows the Single Responsibility Principle by isolating
 * hardware discovery logic from UI rendering.
 */
class V4L2DeviceScanner {
public:
    /**
     * Scan for available V4L2 video capture devices.
     * @return Vector of device paths (e.g., "/dev/video0", "/dev/video1")
     */
    static std::vector<std::string> scan();
    
    /**
     * Scan for available V4L2 video capture devices and populate a vector.
     * @param[out] devices Vector to populate with device paths
     * @return Number of devices found
     */
    static size_t scan(std::vector<std::string>& devices);
    
    /**
     * Check if a specific device path is a valid V4L2 capture device.
     * @param devicePath Device path to check (e.g., "/dev/video0")
     * @return true if device exists and supports video capture, false otherwise
     */
    static bool isValidDevice(const std::string& devicePath);
};

