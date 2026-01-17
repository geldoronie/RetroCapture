#include "OpenGLRenderer.h"
#include "OpenGLStateTracker.h"
#include "glad_loader.h"
#include "../utils/Logger.h"
#ifdef __linux__
#include <linux/videodev2.h>
#else
// Definir constantes V4L2 para Windows (valores padrão)
#define V4L2_PIX_FMT_RGB24  0x52474232  // 'RGB2'
#define V4L2_PIX_FMT_RGB32  0x52474234  // 'RGB4'
#define V4L2_PIX_FMT_BGR32  0x42475232  // 'BGR2'
#define V4L2_PIX_FMT_YUYV   0x56595559  // 'YUYV'
#define V4L2_PIX_FMT_MJPEG  0x47504A4D  // 'MJPG'
#define V4L2_PIX_FMT_JPEG   0x4745504A  // 'JPEG'
#endif
#include <cstring>

// Shaders serão gerados dinamicamente baseados na versão OpenGL
// Funções helper para gerar shaders compatíveis
std::string generateVertexShader()
{
    std::string version = getGLSLVersionString();
    bool isES = isOpenGLES();
    int major = getOpenGLMajorVersion();
    
    // Limpar a string de versão: remover espaços extras e newlines
    // A string deve ser exatamente "#version XXX" sem espaços extras
    while (!version.empty() && (version.back() == '\n' || version.back() == '\r' || version.back() == ' ')) {
        version.pop_back();
    }
    while (!version.empty() && (version.front() == ' ' || version.front() == '\n' || version.front() == '\r')) {
        version.erase(0, 1);
    }
    
    // Verificar se é OpenGL ES - nunca usar "core" em ES
    if (isES && major >= 3) {
        // OpenGL ES 3.0+ - usar in/out (sem "core")
        return version + "\n" +
               "in vec2 aPos;\n"
               "in vec2 aTexCoord;\n"
               "\n"
               "out vec2 TexCoord;\n"
               "\n"
               "void main() {\n"
               "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
               "    TexCoord = aTexCoord;\n"
               "}\n";
    } else if (isES) {
        // OpenGL ES 2.0 - usar attribute/varying
        return version + "\n" +
               "precision mediump float;\n"
               "attribute vec2 aPos;\n"
               "attribute vec2 aTexCoord;\n"
               "\n"
               "varying vec2 TexCoord;\n"
               "\n"
               "void main() {\n"
               "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
               "    TexCoord = aTexCoord;\n"
               "}\n";
    } else if (major >= 3) {
        // OpenGL 3.0+ Desktop - usar layout location com "core"
        return version + " core\n"
               "layout (location = 0) in vec2 aPos;\n"
               "layout (location = 1) in vec2 aTexCoord;\n"
               "\n"
               "out vec2 TexCoord;\n"
               "\n"
               "void main() {\n"
               "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
               "    TexCoord = aTexCoord;\n"
               "}\n";
    } else {
        // OpenGL 2.1 Desktop - usar attribute/varying, sem "core"
        return version + "\n" +
               "attribute vec2 aPos;\n"
               "attribute vec2 aTexCoord;\n"
               "\n"
               "varying vec2 TexCoord;\n"
               "\n"
               "void main() {\n"
               "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
               "    TexCoord = aTexCoord;\n"
               "}\n";
    }
}

std::string generateFragmentShader()
{
    std::string version = getGLSLVersionString();
    bool isES = isOpenGLES();
    int major = getOpenGLMajorVersion();
    
    // Limpar a string de versão: remover espaços extras e newlines
    while (!version.empty() && (version.back() == '\n' || version.back() == '\r' || version.back() == ' ')) {
        version.pop_back();
    }
    while (!version.empty() && (version.front() == ' ' || version.front() == '\n' || version.front() == '\r')) {
        version.erase(0, 1);
    }
    
    // Verificar se é OpenGL ES - nunca usar "core" em ES
    if (isES && major >= 3) {
        // OpenGL ES 3.0+ - usar in/out (sem "core")
        return version + "\n" +
               "precision mediump float;\n"
               "in vec2 TexCoord;\n"
               "\n"
               "out vec4 FragColor;\n"
               "\n"
               "uniform sampler2D ourTexture;\n"
               "uniform int flipY;\n"
               "uniform float brightness;\n"
               "uniform float contrast;\n"
               "\n"
               "void main() {\n"
               "    vec2 coord = TexCoord;\n"
               "    if (flipY == 1) coord.y = 1.0 - coord.y;\n"
               "    vec4 color = texture(ourTexture, coord);\n"
               "    color.rgb = (color.rgb - 0.5) * contrast + 0.5 + brightness - 1.0;\n"
               "    FragColor = color;\n"
               "}\n";
    } else if (isES) {
        // OpenGL ES 2.0 - usar varying e gl_FragColor
        return version + "\n" +
               "precision mediump float;\n"
               "varying vec2 TexCoord;\n"
               "\n"
               "uniform sampler2D ourTexture;\n"
               "uniform int flipY;\n"
               "uniform float brightness;\n"
               "uniform float contrast;\n"
               "\n"
               "void main() {\n"
               "    vec2 coord = TexCoord;\n"
               "    if (flipY == 1) coord.y = 1.0 - coord.y;\n"
               "    vec4 color = texture2D(ourTexture, coord);\n"
               "    color.rgb = (color.rgb - 0.5) * contrast + 0.5 + brightness - 1.0;\n"
               "    gl_FragColor = color;\n"
               "}\n";
    } else if (major >= 3) {
        // OpenGL 3.0+ Desktop - usar in/out com "core"
        return version + " core\n" +
               "in vec2 TexCoord;\n"
               "out vec4 FragColor;\n"
               "\n"
               "uniform sampler2D ourTexture;\n"
               "uniform int flipY;\n"
               "uniform float brightness;\n"
               "uniform float contrast;\n"
               "\n"
               "void main() {\n"
               "    vec2 coord = (flipY == 1) ? vec2(TexCoord.x, 1.0 - TexCoord.y) : TexCoord;\n"
               "    vec4 texColor = texture(ourTexture, coord);\n"
               "    vec3 color = texColor.rgb * brightness;\n"
               "    color = (color - 0.5) * contrast + 0.5;\n"
               "    FragColor = vec4(color, texColor.a);\n"
               "}\n";
    } else {
        // OpenGL 2.1 Desktop - não usar "core" profile
        return version + "\n" +
               "varying vec2 TexCoord;\n"
               "\n"
               "uniform sampler2D ourTexture;\n"
               "uniform int flipY;\n"
               "uniform float brightness;\n"
               "uniform float contrast;\n"
               "\n"
               "void main() {\n"
               "    vec2 coord = (flipY == 1) ? vec2(TexCoord.x, 1.0 - TexCoord.y) : TexCoord;\n"
               "    vec4 texColor = texture2D(ourTexture, coord);\n"
               "    vec3 color = texColor.rgb * brightness;\n"
               "    color = (color - 0.5) * contrast + 0.5;\n"
               "    gl_FragColor = vec4(color, texColor.a);\n"
               "}\n";
    }
}

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
    
    // NOTA: State tracker desabilitado temporariamente
    // O ShaderEngine e outros componentes também fazem binds de texturas,
    // então o state tracker pode causar problemas ao impedir binds necessários
    // TODO: Implementar state tracking mais robusto que sincronize com outros componentes
    // m_stateTracker = std::make_unique<OpenGLStateTracker>();
    
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
    // Gerar shaders dinamicamente baseados na versão OpenGL
    std::string vertexSource = generateVertexShader();
    std::string fragmentSource = generateFragmentShader();
    
    // Debug: logar primeira linha do shader para diagnóstico
    size_t firstNewline = vertexSource.find('\n');
    std::string firstLine = firstNewline != std::string::npos 
        ? vertexSource.substr(0, firstNewline) 
        : vertexSource;
    LOG_INFO("Vertex shader first line: " + firstLine);
    
    const char* vertexSourceCStr = vertexSource.c_str();
    const char* fragmentSourceCStr = fragmentSource.c_str();
    
    // Compilar vertex shader
    GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexSourceCStr, nullptr);
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
    glShaderSource(fragmentShader, 1, &fragmentSourceCStr, nullptr);
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
    // NOTA: State tracker desabilitado temporariamente
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
    // NOTA: State tracker desabilitado temporariamente
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

void OpenGLRenderer::renderTexture(GLuint texture, uint32_t windowWidth, uint32_t windowHeight, bool flipY, bool enableBlend, float brightness, float contrast, bool maintainAspect, uint32_t textureWidth, uint32_t textureHeight) {
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
    // NOTA: State tracker desabilitado temporariamente - pode causar problemas quando
    // outros componentes (ShaderEngine) alteram o estado OpenGL
    // TODO: Implementar state tracking mais robusto que sincronize com outros componentes
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
    
    // Configurar contraste
    GLint contrastLoc = glGetUniformLocation(m_shaderProgram, "contrast");
    if (contrastLoc >= 0) {
        glUniform1f(contrastLoc, contrast);
    }
    
    // IMPORTANT: Always set viewport to full window first (for clearing)
    // Then calculate aspect ratio viewport if needed
    GLint viewportX = 0;
    GLint viewportY = 0;
    GLsizei viewportWidth = windowWidth;
    GLsizei viewportHeight = windowHeight;
    
    if (maintainAspect && textureWidth > 0 && textureHeight > 0) {
        // Calcular aspect ratio da textura e da janela
        float textureAspect = static_cast<float>(textureWidth) / static_cast<float>(textureHeight);
        float windowAspect = static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
        
        // Debug log (mais detalhado para diagnóstico)
        static int aspectLogCount = 0;
        if (aspectLogCount++ < 5) {
            LOG_INFO("=== ASPECT RATIO CALCULATION (renderTexture) ===");
            LOG_INFO("Texture: " + std::to_string(textureWidth) + "x" + std::to_string(textureHeight) + 
                     " (aspect: " + std::to_string(textureAspect) + ")");
            LOG_INFO("Window: " + std::to_string(windowWidth) + "x" + std::to_string(windowHeight) + 
                     " (aspect: " + std::to_string(windowAspect) + ")");
        }
        
        if (textureAspect > windowAspect) {
            // Textura é mais larga: ajustar altura (letterboxing)
            viewportHeight = static_cast<GLsizei>(static_cast<float>(windowWidth) / textureAspect);
            viewportY = (windowHeight - viewportHeight) / 2;
            if (aspectLogCount <= 5) {
                LOG_INFO("Letterboxing: viewport=" + std::to_string(viewportX) + "," + std::to_string(viewportY) + 
                         " " + std::to_string(viewportWidth) + "x" + std::to_string(viewportHeight));
            }
        } else {
            // Textura é mais alta: ajustar largura (pillarboxing)
            viewportWidth = static_cast<GLsizei>(static_cast<float>(windowHeight) * textureAspect);
            viewportX = (windowWidth - viewportWidth) / 2;
            if (aspectLogCount <= 5) {
                LOG_INFO("Pillarboxing: viewport=" + std::to_string(viewportX) + "," + std::to_string(viewportY) + 
                         " " + std::to_string(viewportWidth) + "x" + std::to_string(viewportHeight));
            }
        }
    } else if (maintainAspect) {
        // maintainAspect está ativo mas dimensões inválidas
        static int invalidDimensionLogCount = 0;
        if (invalidDimensionLogCount++ < 3) {
            LOG_WARN("maintainAspect ativo mas dimensões inválidas: " + 
                     std::to_string(textureWidth) + "x" + std::to_string(textureHeight));
        }
    }
    
    // IMPORTANT: Set viewport - this is the final viewport for rendering
    glViewport(viewportX, viewportY, viewportWidth, viewportHeight);
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

