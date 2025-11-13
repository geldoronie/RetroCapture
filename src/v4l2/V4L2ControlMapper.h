#pragma once

#include <string>
#include <cstdint>
#include <linux/videodev2.h>

/**
 * Maps V4L2 control names to their control IDs.
 * 
 * This class extracts the control name-to-ID mapping logic from Application
 * while preserving the exact same behavior.
 */
class V4L2ControlMapper {
public:
    /**
     * Get the V4L2 control ID for a given control name.
     * 
     * @param name Control name (e.g., "Brightness", "Contrast")
     * @return Control ID, or 0 if not found
     */
    static uint32_t getControlId(const std::string& name);

    /**
     * Check if a control name is supported.
     * 
     * @param name Control name
     * @return true if the control is supported, false otherwise
     */
    static bool isControlSupported(const std::string& name);
};

