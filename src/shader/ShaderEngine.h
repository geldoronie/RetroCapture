#pragma once

#include "../renderer/glad_loader.h"
#include "ShaderPreset.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <cstdint>

struct ShaderPassData {
    GLuint program = 0;
    GLuint vertexShader = 0;
    GLuint fragmentShader = 0;
    GLuint framebuffer = 0;
    GLuint texture = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool floatFramebuffer = false;
    ShaderPass passInfo;
    // Parâmetros extraídos de #pragma parameter (nome -> valor padrão)
    std::map<std::string, float> extractedParameters;
};

class ShaderEngine {
public:
    ShaderEngine();
    ~ShaderEngine();
    
    bool init();
    void shutdown();
    
    // Carregar shader simples do RetroArch
    bool loadShader(const std::string& shaderPath);
    
    // Carregar preset com múltiplos passes
    bool loadPreset(const std::string& presetPath);
    
    // Aplicar shader/preset na textura
    GLuint applyShader(GLuint inputTexture, uint32_t width, uint32_t height);
    
    // Desabilitar shader (retorna ao renderizador básico)
    void disableShader();
    
    bool isShaderActive() const { return m_shaderActive; }
    
    // Uniforms do RetroArch
    void setUniform(const std::string& name, float value);
    void setUniform(const std::string& name, float x, float y);
    void setUniform(const std::string& name, float x, float y, float z, float w);
    
private:
    bool m_initialized = false;
    bool m_shaderActive = false;
    
    // Modo simples (shader único)
    GLuint m_shaderProgram = 0;
    GLuint m_vertexShader = 0;
    GLuint m_fragmentShader = 0;
    GLuint m_framebuffer = 0;
    GLuint m_outputTexture = 0;
    uint32_t m_outputWidth = 0;
    uint32_t m_outputHeight = 0;
    
    // Modo preset (múltiplos passes)
    ShaderPreset m_preset;
    std::vector<ShaderPassData> m_passes;
    std::unordered_map<std::string, GLuint> m_textureReferences;
    uint32_t m_sourceWidth = 0;
    uint32_t m_sourceHeight = 0;
    uint32_t m_viewportWidth = 0;
    uint32_t m_viewportHeight = 0;
    
    // VAO para renderização
    GLuint m_VAO = 0;
    GLuint m_VBO = 0;
    GLuint m_EBO = 0;
    
    // Uniforms
    std::unordered_map<std::string, GLint> m_uniformLocations;
    float m_frameCount = 0.0f;
    float m_time = 0.0f;
    
    bool compileShader(const std::string& source, GLenum type, GLuint& shader);
    bool linkProgram(GLuint vertexShader, GLuint fragmentShader);
    GLint getUniformLocation(GLuint program, const std::string& name);
    void createFramebuffer(uint32_t width, uint32_t height, bool floatBuffer, GLuint& fb, GLuint& tex);
    void cleanupFramebuffer(GLuint& fb, GLuint& tex);
    void createQuad();
    void cleanupQuad();
    
    // Gerar vertex shader padrão do RetroArch
    std::string generateDefaultVertexShader();
    
    // Preset functions
    bool loadPresetPasses();
    void cleanupPresetPasses();
    uint32_t calculateScale(uint32_t sourceSize, const std::string& scaleType, float scale, 
                           uint32_t viewportSize, uint32_t absoluteValue);
    void setupUniforms(GLuint program, uint32_t passIndex, uint32_t inputWidth, uint32_t inputHeight,
                      uint32_t outputWidth, uint32_t outputHeight);
    bool loadTextureReference(const std::string& name, const std::string& path);
    void cleanupTextureReferences();
    
    // Conversão Slang para GLSL
    std::string convertSlangToGLSL(const std::string& slangSource, bool isVertex, const std::string& basePath = "");
    std::string processIncludes(const std::string& source, const std::string& basePath);
};

