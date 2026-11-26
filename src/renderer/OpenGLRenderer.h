#pragma once

#include "glad_loader.h"
#include <cstdint>
#include <vector>
#include <memory>
#include <string>

// Vertex shader for a full-screen textured quad (attributes aPos/aTexCoord,
// varying TexCoord) in the GLSL dialect of the current context. Shared with
// other GPU passes (SourcePrePass) so they stay dialect-compatible.
std::string generateVertexShader();

// Forward declaration
class OpenGLStateTracker;

class OpenGLRenderer {
public:
    OpenGLRenderer();
    ~OpenGLRenderer();
    
    bool init();
    void shutdown();
    
    // Criar textura a partir de dados de frame
    GLuint createTextureFromFrame(const uint8_t* data, uint32_t width, uint32_t height, uint32_t format);
    
    // Atualizar textura com novo frame
    void updateTexture(GLuint texture, const uint8_t* data, uint32_t width, uint32_t height, uint32_t format);
    
    // Renderizar textura na tela
    // Se maintainAspect=true, calcula viewport mantendo proporção da textura.
    // Se preserveViewport=true, NÃO sobrescreve o viewport (o caller já configurou,
    // ex: downscale com overscan via offset negativo).
    void renderTexture(GLuint texture, uint32_t windowWidth, uint32_t windowHeight,
                       bool flipY = true, bool enableBlend = false,
                       float brightness = 1.0f, float contrast = 1.0f,
                       bool maintainAspect = false, uint32_t textureWidth = 0, uint32_t textureHeight = 0,
                       bool preserveViewport = false);
    
    // #227 — texture-coordinate transform (rotation / mirror) applied by
    // renderTexture(), row-major 2x2 around the texture centre. Stays in
    // effect until reset; callers set it for one draw and reset right after
    // so every other renderTexture() user keeps the identity.
    void setUVTransform(const float m[4])
    {
        for (int i = 0; i < 4; ++i) m_uvXform[i] = m[i];
    }
    void resetUVTransform()
    {
        m_uvXform[0] = 1.0f; m_uvXform[1] = 0.0f;
        m_uvXform[2] = 0.0f; m_uvXform[3] = 1.0f;
    }

    // Limpar tela
    void clear(float r = 0.0f, float g = 0.0f, float b = 0.0f, float a = 1.0f);
    
private:
    bool m_initialized = false;
    float m_uvXform[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    
    // Shader program para renderizar textura
    GLuint m_shaderProgram = 0;
    GLuint m_VAO = 0;
    GLuint m_VBO = 0;
    GLuint m_EBO = 0;
    
    // State tracker para evitar mudanças desnecessárias de estado OpenGL
    std::unique_ptr<OpenGLStateTracker> m_stateTracker;
    
    bool createShaderProgram();
    void createQuad();
    void cleanup();
    
    // Conversão de formatos V4L2 para OpenGL
    GLenum getGLFormat(uint32_t v4l2Format);
    GLenum getGLInternalFormat(uint32_t v4l2Format);
};

