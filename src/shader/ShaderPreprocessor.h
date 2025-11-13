#pragma once

#include <string>
#include <map>
#include <vector>
#include <cstdint>
#include "ShaderPreset.h"

// Forward declarations
struct ShaderParameterInfo;

/**
 * Preprocesses GLSL shader source code.
 * 
 * This class extracts shader preprocessing logic from ShaderEngine
 * while preserving the exact same behavior.
 * 
 * IMPORTANT: This class maintains the working approach:
 * - Uses the SAME source code for both vertex and fragment shaders
 * - Adds #define VERTEX or #define FRAGMENT before the code
 * - Lets the GLSL preprocessor handle conditional blocks
 */
class ShaderPreprocessor {
public:
    /**
     * Result of preprocessing a shader.
     */
    struct PreprocessResult {
        std::string vertexSource;      // Processed vertex shader source
        std::string fragmentSource;    // Processed fragment shader source
        std::map<std::string, float> extractedParameters;  // Parameter name -> default value
        std::map<std::string, ShaderParameterInfo> parameterInfo;  // Full parameter info
    };

    /**
     * Preprocess shader source code.
     * 
     * This method preserves the exact same logic as the original implementation:
     * - Processes includes
     * - Extracts #pragma parameter directives
     * - Corrects OutputSize type
     * - Uses same source for vertex and fragment (with different defines)
     * - Injects compatibility code when needed
     * 
     * @param shaderSource Original shader source code
     * @param shaderPath Path to shader file (for resolving includes)
     * @param passIndex Pass index (for logging)
     * @param outputWidth Output width (for compatibility code injection)
     * @param outputHeight Output height (for compatibility code injection)
     * @param inputWidth Input width (for compatibility code injection)
     * @param inputHeight Input height (for compatibility code injection)
     * @param presetPasses Vector of preset pass info (for checking scale types)
     * @return PreprocessResult with processed sources and extracted parameters
     */
    static PreprocessResult preprocess(
        const std::string& shaderSource,
        const std::string& shaderPath,
        size_t passIndex,
        uint32_t outputWidth,
        uint32_t outputHeight,
        uint32_t inputWidth,
        uint32_t inputHeight,
        const std::vector<ShaderPass>& presetPasses);

    /**
     * Process #include directives.
     * Public so it can be used by ShaderEngine for simple shader loading.
     */
    static std::string processIncludes(const std::string& source, const std::string& basePath);

private:

    /**
     * Extract #pragma parameter directives and remove them from source.
     */
    static std::string extractParameters(
        const std::string& source,
        const std::string& shaderPath,
        std::map<std::string, float>& paramDefaults,
        std::map<std::string, ShaderParameterInfo>& parameterInfo);

    /**
     * Correct OutputSize uniform type based on usage in shader.
     */
    static std::string correctOutputSizeType(const std::string& source);

    /**
     * Build final shader source with version, extensions, and defines.
     * Uses the SAME code for both vertex and fragment, with different defines.
     */
    static std::string buildFinalSource(
        const std::string& code,
        bool isVertex);

    /**
     * Inject compatibility code for specific shaders (interlacing, box-center, etc.).
     */
    static void injectCompatibilityCode(
        std::string& vertexCode,
        std::string& fragmentCode,
        const std::string& shaderPath,
        size_t passIndex,
        uint32_t outputWidth,
        uint32_t outputHeight,
        uint32_t inputWidth,
        uint32_t inputHeight,
        const std::vector<ShaderPass>& presetPasses);
};

