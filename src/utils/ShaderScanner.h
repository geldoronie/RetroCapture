#pragma once

#include <string>
#include <vector>

/**
 * Scans filesystem for shader preset files (.glslp).
 * 
 * This class follows the Single Responsibility Principle by isolating
 * file system scanning logic from UI rendering.
 */
class ShaderScanner {
public:
    /**
     * Scan a directory for shader preset files.
     * @param basePath Base directory path to scan
     * @return Vector of relative paths to .glslp files
     */
    static std::vector<std::string> scan(const std::string& basePath);
    
    /**
     * Scan a directory for shader preset files and populate a vector.
     * @param basePath Base directory path to scan
     * @param[out] shaders Vector to populate with relative paths to .glslp files
     * @return Number of shaders found
     */
    static size_t scan(const std::string& basePath, std::vector<std::string>& shaders);
};

