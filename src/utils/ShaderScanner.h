#pragma once

#include <string>
#include <vector>

/**
 * Scans for shader preset files (.glslp) in a directory.
 * 
 * This class extracts shader scanning logic from UIManager
 * while preserving the exact same behavior.
 */
class ShaderScanner {
public:
    /**
     * Scan for shader presets in a directory.
     * 
     * @param basePath Base path to scan for shaders
     * @return Vector of relative paths to shader presets (relative to basePath)
     */
    static std::vector<std::string> scan(const std::string& basePath);
};

