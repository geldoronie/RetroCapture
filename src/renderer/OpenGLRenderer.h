#pragma once

#include "glad_loader.h"
#include <cstdint>
#include <vector>

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
    void renderTexture(GLuint texture, uint32_t windowWidth, uint32_t windowHeight, bool flipY = true);
    
    // Limpar tela
    void clear(float r = 0.0f, float g = 0.0f, float b = 0.0f, float a = 1.0f);
    
private:
    bool m_initialized = false;
    
    // Shader program para renderizar textura
    GLuint m_shaderProgram = 0;
    GLuint m_VAO = 0;
    GLuint m_VBO = 0;
    GLuint m_EBO = 0;
    
    bool createShaderProgram();
    void createQuad();
    void cleanup();
    
    // Conversão de formatos V4L2 para OpenGL
    GLenum getGLFormat(uint32_t v4l2Format);
    GLenum getGLInternalFormat(uint32_t v4l2Format);
};

