#pragma once

#include "../renderer/glad_loader.h"
#include "ShaderPreset.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <cstdint>

struct ShaderParameterInfo {
    float defaultValue;
    float min;
    float max;
    float step;
    std::string description;
};

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
    // Informações completas dos parâmetros (nome -> info)
    std::map<std::string, ShaderParameterInfo> parameterInfo;
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
    
    // Atualizar viewport (dimensões da janela)
    void setViewport(uint32_t width, uint32_t height);
    
    // Desabilitar shader (retorna ao renderizador básico)
    void disableShader();
    
    bool isShaderActive() const { return m_shaderActive; }
    
    // Obter dimensões de saída do shader
    uint32_t getOutputWidth() const { return m_outputWidth; }
    uint32_t getOutputHeight() const { return m_outputHeight; }
    
    // Uniforms do RetroArch
    void setUniform(const std::string& name, float value);
    void setUniform(const std::string& name, float x, float y);
    void setUniform(const std::string& name, float x, float y, float z, float w);
    
    // Parâmetros de shader (extraídos de #pragma parameter)
    struct ShaderParameter {
        std::string name;
        float value;
        float defaultValue;
        float min;
        float max;
        float step;
        std::string description;
    };
    std::vector<ShaderParameter> getShaderParameters() const;
    bool setShaderParameter(const std::string& name, float value);
    
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
    
    // Parâmetros customizados (valores alterados pelo usuário)
    std::map<std::string, float> m_customParameters;
    
    // Histórico de frames para motion blur (mantém até 7 frames anteriores)
    std::vector<GLuint> m_frameHistory; // Texturas de frames anteriores
    std::vector<uint32_t> m_frameHistoryWidths;
    std::vector<uint32_t> m_frameHistoryHeights;
    static constexpr size_t MAX_FRAME_HISTORY = 7;
    
    bool compileShader(const std::string& source, GLenum type, GLuint& shader);
    bool linkProgram(GLuint vertexShader, GLuint fragmentShader);
    GLint getUniformLocation(GLuint program, const std::string& name);
    void createFramebuffer(uint32_t width, uint32_t height, bool floatBuffer, GLuint& fb, GLuint& tex, bool srgbBuffer = false);
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
    
    // Funções auxiliares para aplicar configurações de textura
    GLenum wrapModeToGLEnum(const std::string& wrapMode);
    void applyTextureSettings(GLuint texture, bool filterLinear, const std::string& wrapMode, bool generateMipmap = false);
    
    // Conversão Slang para GLSL
    std::string convertSlangToGLSL(const std::string& slangSource, bool isVertex, const std::string& basePath = "");
    std::string processIncludes(const std::string& source, const std::string& basePath);
};

