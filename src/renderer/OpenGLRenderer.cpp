#include "OpenGLRenderer.h"
#include "glad_loader.h"
#include "../utils/Logger.h"
#include <linux/videodev2.h>
#include <cstring>

// Shader vertex simples
const char* vertexShaderSource = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoord;

out vec2 TexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

// Shader fragment simples
const char* fragmentShaderSource = R"(
#version 330 core
in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D ourTexture;
uniform int flipY;
uniform float brightness;

void main() {
    // Inverter coordenada Y apenas se flipY estiver ativo
    vec2 coord = (flipY == 1) ? vec2(TexCoord.x, 1.0 - TexCoord.y) : TexCoord;
    vec4 texColor = texture(ourTexture, coord);
    
    // Aplicar brilho multiplicando RGB pelo fator de brilho
    // Preservar alpha para shaders que usam transparência
    FragColor = vec4(texColor.rgb * brightness, texColor.a);
}
)";

OpenGLRenderer::OpenGLRenderer() {
}

OpenGLRenderer::~OpenGLRenderer() {
    shutdown();
}

bool OpenGLRenderer::init() {
    if (m_initialized) {
        return true;
    }
    
    // Carregar funções OpenGL
    if (!loadOpenGLFunctions()) {
        LOG_ERROR("Falha ao carregar funções OpenGL");
        return false;
    }
    
    if (!createShaderProgram()) {
        return false;
    }
    
    createQuad();
    
    m_initialized = true;
    LOG_INFO("OpenGLRenderer inicializado");
    return true;
}

void OpenGLRenderer::shutdown() {
    if (!m_initialized) {
        return;
    }
    
    cleanup();
    m_initialized = false;
    LOG_INFO("OpenGLRenderer encerrado");
}

bool OpenGLRenderer::createShaderProgram() {
    // Compilar vertex shader
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexShaderSource, nullptr);
    glCompileShader(vertexShader);
    
    GLint success;
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(vertexShader, 512, nullptr, infoLog);
        LOG_ERROR("Erro ao compilar vertex shader: " + std::string(infoLog));
        glDeleteShader(vertexShader);
        return false;
    }
    
    // Compilar fragment shader
    GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentShaderSource, nullptr);
    glCompileShader(fragmentShader);
    
    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(fragmentShader, 512, nullptr, infoLog);
        LOG_ERROR("Erro ao compilar fragment shader: " + std::string(infoLog));
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return false;
    }
    
    // Criar shader program
    m_shaderProgram = glCreateProgram();
    glAttachShader(m_shaderProgram, vertexShader);
    glAttachShader(m_shaderProgram, fragmentShader);
    glLinkProgram(m_shaderProgram);
    
    glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(m_shaderProgram, 512, nullptr, infoLog);
        LOG_ERROR("Erro ao linkar shader program: " + std::string(infoLog));
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        glDeleteProgram(m_shaderProgram);
        m_shaderProgram = 0;
        return false;
    }
    
    // Limpar shaders (já linkados)
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    
    return true;
}

void OpenGLRenderer::createQuad() {
    // Quad para renderizar textura (coordenadas normalizadas)
    float vertices[] = {
        // Posições      // TexCoords
        -1.0f, -1.0f,    0.0f, 0.0f,
         1.0f, -1.0f,    1.0f, 0.0f,
         1.0f,  1.0f,    1.0f, 1.0f,
        -1.0f,  1.0f,    0.0f, 1.0f
    };
    
    unsigned int indices[] = {
        0, 1, 2,
        2, 3, 0
    };
    
    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glGenBuffers(1, &m_EBO);
    
    glBindVertexArray(m_VAO);
    
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    
    // Posição
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    // Coordenadas de textura
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    glBindVertexArray(0);
}

void OpenGLRenderer::cleanup() {
    if (m_VAO) {
        glDeleteVertexArrays(1, &m_VAO);
        m_VAO = 0;
    }
    if (m_VBO) {
        glDeleteBuffers(1, &m_VBO);
        m_VBO = 0;
    }
    if (m_EBO) {
        glDeleteBuffers(1, &m_EBO);
        m_EBO = 0;
    }
    if (m_shaderProgram) {
        glDeleteProgram(m_shaderProgram);
        m_shaderProgram = 0;
    }
}

GLuint OpenGLRenderer::createTextureFromFrame(const uint8_t* data, uint32_t width, uint32_t height, uint32_t format) {
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    
    // Configurações de textura
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    
    updateTexture(texture, data, width, height, format);
    
    return texture;
}

void OpenGLRenderer::updateTexture(GLuint texture, const uint8_t* data, uint32_t width, uint32_t height, uint32_t format) {
    glBindTexture(GL_TEXTURE_2D, texture);
    
    GLenum glFormat = getGLFormat(format);
    GLenum glInternalFormat = getGLInternalFormat(format);
    
    if (format == V4L2_PIX_FMT_YUYV) {
        // YUYV precisa ser convertido ou usado com formato especial
        // Por enquanto, vamos usar GL_RGB e converter depois se necessário
        // Para MVP, vamos assumir que podemos usar GL_RGB diretamente
        // (na prática, precisaria de conversão)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
    } else if (format == V4L2_PIX_FMT_MJPEG || format == V4L2_PIX_FMT_JPEG) {
        // MJPEG precisa ser decodificado primeiro
        LOG_WARN("MJPEG precisa ser decodificado antes de criar textura");
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, glInternalFormat, width, height, 0, glFormat, GL_UNSIGNED_BYTE, data);
    }
}

void OpenGLRenderer::renderTexture(GLuint texture, uint32_t windowWidth, uint32_t windowHeight, bool flipY, bool enableBlend, float brightness) {
    // Verificar se a textura é válida
    if (texture == 0) {
        LOG_ERROR("Tentativa de renderizar textura inválida (0)");
        return;
    }
    
    // IMPORTANTE: Habilitar blending ANTES de usar o shader program
    // Para texturas com alpha (como shaders Game Boy), precisamos de blending
    // O blending permite que pixels com alpha 0 sejam transparentes
    if (enableBlend) {
        glEnable(GL_BLEND);
        // GL_SRC_ALPHA: usa o alpha da textura fonte
        // GL_ONE_MINUS_SRC_ALPHA: usa (1 - alpha) para o destino
        // Isso cria o efeito de transparência corretamente
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    
    glUseProgram(m_shaderProgram);
    glBindVertexArray(m_VAO);
    
    // Bind da textura na unidade 0
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    
    // Configurar o sampler uniform (o shader usa "ourTexture")
    GLint texLoc = glGetUniformLocation(m_shaderProgram, "ourTexture");
    if (texLoc >= 0) {
        glUniform1i(texLoc, 0);
    }
    
    // Configurar se devemos inverter Y
    GLint flipYLoc = glGetUniformLocation(m_shaderProgram, "flipY");
    if (flipYLoc >= 0) {
        glUniform1i(flipYLoc, flipY ? 1 : 0);
    }
    
    // Configurar brilho
    GLint brightnessLoc = glGetUniformLocation(m_shaderProgram, "brightness");
    if (brightnessLoc >= 0) {
        glUniform1f(brightnessLoc, brightness);
    }
    
    glViewport(0, 0, windowWidth, windowHeight);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OpenGLRenderer::clear(float r, float g, float b, float a) {
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}

GLenum OpenGLRenderer::getGLFormat(uint32_t v4l2Format) {
    switch (v4l2Format) {
        case V4L2_PIX_FMT_RGB24:
            return GL_RGB;
        case V4L2_PIX_FMT_RGB32:
        case V4L2_PIX_FMT_BGR32:
            return GL_RGBA;
        case V4L2_PIX_FMT_YUYV:
            return GL_RGB; // Precisa conversão
        default:
            return GL_RGB;
    }
}

GLenum OpenGLRenderer::getGLInternalFormat(uint32_t v4l2Format) {
    switch (v4l2Format) {
        case V4L2_PIX_FMT_RGB24:
            return GL_RGB;
        case V4L2_PIX_FMT_RGB32:
        case V4L2_PIX_FMT_BGR32:
            return GL_RGBA;
        case V4L2_PIX_FMT_YUYV:
            return GL_RGB; // Precisa conversão
        default:
            return GL_RGB;
    }
}

