#include "ShaderEngine.h"
#include "../utils/Logger.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <cmath>
#include <cstring>
#include <regex>
#include <set>

ShaderEngine::ShaderEngine() {
}

ShaderEngine::~ShaderEngine() {
    shutdown();
}

bool ShaderEngine::init() {
    if (m_initialized) {
        return true;
    }
    
    createQuad();
    m_initialized = true;
    LOG_INFO("ShaderEngine inicializado");
    return true;
}

void ShaderEngine::shutdown() {
    if (!m_initialized) {
        return;
    }
    
    disableShader();
    cleanupPresetPasses();
    cleanupTextureReferences();
    cleanupQuad();
    
    m_initialized = false;
    LOG_INFO("ShaderEngine encerrado");
}

std::string ShaderEngine::generateDefaultVertexShader() {
    // Vertex shader compatível com RetroArch
    // Não usar layout(location = X) pois foi removido na conversão
    // Usar vec4 Position para compatibilidade com RetroArch (que usa vec4)
    return R"(#version 330 core
in vec4 Position;
in vec2 TexCoord;

out vec2 vTexCoord;

void main() {
    gl_Position = Position;
    vTexCoord = TexCoord;
}
)";
}

bool ShaderEngine::loadShader(const std::string& shaderPath) {
    if (!m_initialized) {
        LOG_ERROR("ShaderEngine não inicializado");
        return false;
    }
    
    disableShader();
    
    std::ifstream file(shaderPath);
    if (!file.is_open()) {
        LOG_ERROR("Falha ao abrir shader: " + shaderPath);
        return false;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string fragmentSource = buffer.str();
    file.close();
    
    // Extrair diretório base do shader para resolver includes
    std::filesystem::path shaderFilePath(shaderPath);
    std::string shaderDir = shaderFilePath.parent_path().string();
    
    // Converter Slang para GLSL se necessário (processa includes também)
    fragmentSource = convertSlangToGLSL(fragmentSource, false, shaderDir);
    
    std::string vertexSource = generateDefaultVertexShader();
    
    GLuint vertexShader = 0;
    GLuint fragmentShader = 0;
    
    if (!compileShader(vertexSource, GL_VERTEX_SHADER, vertexShader)) {
        return false;
    }
    
    if (!compileShader(fragmentSource, GL_FRAGMENT_SHADER, fragmentShader)) {
        glDeleteShader(vertexShader);
        return false;
    }
    
    disableShader();
    
    if (!linkProgram(vertexShader, fragmentShader)) {
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return false;
    }
    
    m_vertexShader = vertexShader;
    m_fragmentShader = fragmentShader;
    m_shaderActive = true;
    m_uniformLocations.clear();
    
    LOG_INFO("Shader carregado: " + shaderPath);
    return true;
}

bool ShaderEngine::loadPreset(const std::string& presetPath) {
    if (!m_initialized) {
        LOG_ERROR("ShaderEngine não inicializado");
        return false;
    }
    
    disableShader();
    cleanupPresetPasses();
    
    if (!m_preset.load(presetPath)) {
        return false;
    }
    
    // Carregar texturas de referência
    const auto& textures = m_preset.getTextures();
    for (const auto& tex : textures) {
        loadTextureReference(tex.first, tex.second.path);
    }
    
    m_shaderActive = true;
    LOG_INFO("Preset carregado: " + presetPath);
    return true;
}

bool ShaderEngine::loadPresetPasses() {
    cleanupPresetPasses();
    
    const auto& passes = m_preset.getPasses();
    m_passes.resize(passes.size());
    
    for (size_t i = 0; i < passes.size(); ++i) {
        const auto& passInfo = passes[i];
        auto& passData = m_passes[i];
        passData.passInfo = passInfo;
        
        // Ler shader
        std::ifstream file(passInfo.shaderPath);
        if (!file.is_open()) {
            LOG_ERROR("Falha ao abrir shader do pass " + std::to_string(i) + ": " + passInfo.shaderPath);
            cleanupPresetPasses();
            return false;
        }
        
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string shaderSource = buffer.str();
        file.close();
        
        // Extrair diretório base do shader para resolver includes
        std::filesystem::path shaderPath(passInfo.shaderPath);
        std::string shaderDir = shaderPath.parent_path().string();
        
        // Converter Slang para GLSL se necessário (processa includes também)
        // Agora extraímos AMBOS vertex e fragment do mesmo arquivo
        std::string vertexSource = convertSlangToGLSL(shaderSource, true, shaderDir);
        std::string fragmentSource = convertSlangToGLSL(shaderSource, false, shaderDir);
        
        
        // Compilar shaders
        if (!compileShader(vertexSource, GL_VERTEX_SHADER, passData.vertexShader)) {
            LOG_ERROR("Falha ao compilar vertex shader do pass " + std::to_string(i));
            cleanupPresetPasses();
            return false;
        }
        
        if (!compileShader(fragmentSource, GL_FRAGMENT_SHADER, passData.fragmentShader)) {
            LOG_ERROR("Falha ao compilar fragment shader do pass " + std::to_string(i));
            glDeleteShader(passData.vertexShader);
            cleanupPresetPasses();
            return false;
        }
        
        // Criar e linkar programa para este pass (cada pass precisa de seu próprio programa)
        GLuint program = glCreateProgram();
        if (program == 0) {
            LOG_ERROR("Falha ao criar shader program do pass " + std::to_string(i));
            cleanupPresetPasses();
            return false;
        }
        
        glAttachShader(program, passData.vertexShader);
        glAttachShader(program, passData.fragmentShader);
        
        // Ligar atributos antes de linkar (necessário quando não usamos layout(location))
        glBindAttribLocation(program, 0, "Position");
        glBindAttribLocation(program, 1, "TexCoord");
        
        glLinkProgram(program);
        
        GLint success;
        glGetProgramiv(program, GL_LINK_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetProgramInfoLog(program, 512, nullptr, infoLog);
            LOG_ERROR("Erro ao linkar shader program do pass " + std::to_string(i) + ": " + std::string(infoLog));
            glDeleteProgram(program);
            glDeleteShader(passData.vertexShader);
            glDeleteShader(passData.fragmentShader);
            cleanupPresetPasses();
            return false;
        }
        
        passData.program = program;
        passData.floatFramebuffer = passInfo.floatFramebuffer;
    }
    
    LOG_INFO("Preset passes carregados: " + std::to_string(m_passes.size()));
    return true;
}

GLuint ShaderEngine::applyShader(GLuint inputTexture, uint32_t width, uint32_t height) {
    if (!m_shaderActive) {
        return inputTexture;
    }
    
    // Se temos preset, usar modo múltiplos passes
    if (!m_passes.empty() || !m_preset.getPasses().empty()) {
        // Se passes ainda não foram carregados, carregar agora
        if (m_passes.empty()) {
            if (!loadPresetPasses()) {
                return inputTexture;
            }
        }
        
        m_sourceWidth = width;
        m_sourceHeight = height;
        m_viewportWidth = width;
        m_viewportHeight = height;
        
        GLuint currentTexture = inputTexture;
        uint32_t currentWidth = width;
        uint32_t currentHeight = height;
        
        // Aplicar cada pass
        for (size_t i = 0; i < m_passes.size(); ++i) {
            auto& pass = m_passes[i];
            const auto& passInfo = pass.passInfo;
            
            // Calcular dimensões de saída
            // Para absolute, precisamos ler o valor do preset
            uint32_t absX = 0, absY = 0;
            if (passInfo.scaleTypeX == "absolute") {
                absX = static_cast<uint32_t>(std::round(passInfo.scaleX));
            }
            if (passInfo.scaleTypeY == "absolute") {
                absY = static_cast<uint32_t>(std::round(passInfo.scaleY));
            }
            
            uint32_t outputWidth = calculateScale(currentWidth, passInfo.scaleTypeX, passInfo.scaleX,
                                                 m_viewportWidth, absX);
            uint32_t outputHeight = calculateScale(currentHeight, passInfo.scaleTypeY, passInfo.scaleY,
                                                  m_viewportHeight, absY);
            
            // Criar/atualizar framebuffer se necessário
            if (pass.framebuffer == 0 || pass.width != outputWidth || pass.height != outputHeight) {
                cleanupFramebuffer(pass.framebuffer, pass.texture);
                createFramebuffer(outputWidth, outputHeight, pass.floatFramebuffer, 
                                pass.framebuffer, pass.texture);
                pass.width = outputWidth;
                pass.height = outputHeight;
            }
            
            // Bind framebuffer
            glBindFramebuffer(GL_FRAMEBUFFER, pass.framebuffer);
            
            glViewport(0, 0, outputWidth, outputHeight);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);  // Preto agora
            glClear(GL_COLOR_BUFFER_BIT);
            
            // Usar shader program
            glUseProgram(pass.program);
            
            // Verificar se o programa é válido
            if (pass.program == 0) {
                LOG_ERROR("Programa de shader inválido no pass " + std::to_string(i));
            }
            
            // Configurar uniforms
            setupUniforms(pass.program, i, currentWidth, currentHeight, outputWidth, outputHeight);
            
            // Bind texturas
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, currentTexture);
            
            // Verificar se a textura é válida
            if (currentTexture == 0) {
                LOG_ERROR("Textura de entrada inválida no pass " + std::to_string(i));
            }
            
            // Bind texturas de referência
            int texUnit = 1;
            for (const auto& texRef : m_textureReferences) {
                glActiveTexture(GL_TEXTURE0 + texUnit);
                glBindTexture(GL_TEXTURE_2D, texRef.second);
                GLint loc = getUniformLocation(pass.program, texRef.first);
                if (loc >= 0) {
                    glUniform1i(loc, texUnit);
                }
                texUnit++;
            }
            
            // Renderizar
            glBindVertexArray(m_VAO);
            glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
            glBindVertexArray(0);
            
            // Próximo pass usa a saída deste
            currentTexture = pass.texture;
            currentWidth = outputWidth;
            currentHeight = outputHeight;
        }
        
        // Desvincular framebuffer após todos os passes
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        
        glBindTexture(GL_TEXTURE_2D, 0);
        glUseProgram(0);
        
        return currentTexture;
    } else {
        // Modo simples (shader único)
        if (m_framebuffer == 0 || m_outputWidth != width || m_outputHeight != height) {
            cleanupFramebuffer(m_framebuffer, m_outputTexture);
            createFramebuffer(width, height, false, m_framebuffer, m_outputTexture);
            m_outputWidth = width;
            m_outputHeight = height;
        }
        
        glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
        glViewport(0, 0, width, height);
        
        glUseProgram(m_shaderProgram);
        
        GLint texLoc = getUniformLocation(m_shaderProgram, "Texture");
        if (texLoc >= 0) {
            glUniform1i(texLoc, 0);
        }
        
        GLint texSizeLoc = getUniformLocation(m_shaderProgram, "TextureSize");
        if (texSizeLoc >= 0) {
            glUniform2f(texSizeLoc, static_cast<float>(width), static_cast<float>(height));
        }
        
        GLint inputSizeLoc = getUniformLocation(m_shaderProgram, "InputSize");
        if (inputSizeLoc >= 0) {
            glUniform2f(inputSizeLoc, static_cast<float>(width), static_cast<float>(height));
        }
        
        GLint outputSizeLoc = getUniformLocation(m_shaderProgram, "OutputSize");
        if (outputSizeLoc >= 0) {
            glUniform2f(outputSizeLoc, static_cast<float>(width), static_cast<float>(height));
        }
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, inputTexture);
        
        glBindVertexArray(m_VAO);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
        
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        
        return m_outputTexture;
    }
}

uint32_t ShaderEngine::calculateScale(uint32_t sourceSize, const std::string& scaleType, float scale,
                                      uint32_t viewportSize, uint32_t /*absoluteValue*/) {
    if (scaleType == "source") {
        return static_cast<uint32_t>(std::round(sourceSize * scale));
    } else if (scaleType == "viewport") {
        return static_cast<uint32_t>(std::round(viewportSize * scale));
    } else if (scaleType == "absolute") {
        // Para absolute, o valor está em scale (como "800" no preset)
        return static_cast<uint32_t>(std::round(scale));
    }
    return sourceSize;
}

void ShaderEngine::setupUniforms(GLuint program, uint32_t /*passIndex*/, uint32_t inputWidth, uint32_t inputHeight,
                                uint32_t outputWidth, uint32_t outputHeight) {
    // Texture/Source (sempre na unidade 0)
    // RetroArch shaders podem usar "Texture" ou "Source"
    GLint loc = getUniformLocation(program, "Texture");
    if (loc >= 0) {
        glUniform1i(loc, 0);
    }

    loc = getUniformLocation(program, "Source");
    if (loc >= 0) {
        glUniform1i(loc, 0);
    }
    
    // SourceSize (vec4 do RetroArch - convertido de params.SourceSize)
    loc = getUniformLocation(program, "SourceSize");
    if (loc >= 0) {
        glUniform4f(loc, static_cast<float>(inputWidth), static_cast<float>(inputHeight),
                    1.0f / static_cast<float>(inputWidth), 1.0f / static_cast<float>(inputHeight));
    }

    // OriginalSize (vec4 do RetroArch - convertido de params.OriginalSize)
    loc = getUniformLocation(program, "OriginalSize");
    if (loc >= 0) {
        glUniform4f(loc, static_cast<float>(m_sourceWidth), static_cast<float>(m_sourceHeight),
                    1.0f / static_cast<float>(m_sourceWidth), 1.0f / static_cast<float>(m_sourceHeight));
    }

    // OutputSize (vec4 do RetroArch - convertido de params.OutputSize)
    loc = getUniformLocation(program, "OutputSize");
    if (loc >= 0) {
        glUniform4f(loc, static_cast<float>(outputWidth), static_cast<float>(outputHeight),
                    1.0f / static_cast<float>(outputWidth), 1.0f / static_cast<float>(outputHeight));
    }

    // FrameCount (uint convertido para float - convertido de params.FrameCount)
    loc = getUniformLocation(program, "FrameCount");
    if (loc >= 0) {
        glUniform1f(loc, m_frameCount);
    }
    
    // Parâmetros customizados do shader com valores padrão
    // Esses valores serão usados se o shader não definir seus próprios
    loc = getUniformLocation(program, "BLURSCALEX");
    if (loc >= 0) {
        glUniform1f(loc, 0.30f);
    }
    
    loc = getUniformLocation(program, "LOWLUMSCAN");
    if (loc >= 0) {
        glUniform1f(loc, 6.0f);
    }
    
    loc = getUniformLocation(program, "HILUMSCAN");
    if (loc >= 0) {
        glUniform1f(loc, 8.0f);
    }
    
    loc = getUniformLocation(program, "BRIGHTBOOST");
    if (loc >= 0) {
        glUniform1f(loc, 1.25f);
    }
    
    loc = getUniformLocation(program, "MASK_DARK");
    if (loc >= 0) {
        glUniform1f(loc, 0.25f);
    }
    
    loc = getUniformLocation(program, "MASK_FADE");
    if (loc >= 0) {
        glUniform1f(loc, 0.8f);
    }
    
    // TextureSize (vec2 alternativo)
    loc = getUniformLocation(program, "TextureSize");
    if (loc >= 0) {
        glUniform2f(loc, static_cast<float>(inputWidth), static_cast<float>(inputHeight));
    }
    
    // InputSize (vec2 alternativo)
    loc = getUniformLocation(program, "InputSize");
    if (loc >= 0) {
        glUniform2f(loc, static_cast<float>(inputWidth), static_cast<float>(inputHeight));
    }
    
    // VideoSize (tamanho original)
    loc = getUniformLocation(program, "IN.video_size");
    if (loc >= 0) {
        glUniform2f(loc, static_cast<float>(m_sourceWidth), static_cast<float>(m_sourceHeight));
    }
    
    // TextureSize (alternativo)
    loc = getUniformLocation(program, "IN.texture_size");
    if (loc >= 0) {
        glUniform2f(loc, static_cast<float>(inputWidth), static_cast<float>(inputHeight));
    }
    
    // OutputSize (alternativo)
    loc = getUniformLocation(program, "IN.output_size");
    if (loc >= 0) {
        glUniform2f(loc, static_cast<float>(outputWidth), static_cast<float>(outputHeight));
    }
    
    // Frame count e time
    m_frameCount += 1.0f;
    m_time += 0.016f; // ~60fps
    
    loc = getUniformLocation(program, "IN.frame_count");
    if (loc >= 0) {
        glUniform1f(loc, m_frameCount);
    }
    
    loc = getUniformLocation(program, "FRAMEINDEX");
    if (loc >= 0) {
        glUniform1f(loc, m_frameCount);
    }
    
    loc = getUniformLocation(program, "TIME");
    if (loc >= 0) {
        glUniform1f(loc, m_time);
    }
    
    // Parâmetros globais do preset
    const auto& params = m_preset.getParameters();
    for (const auto& param : params) {
        loc = getUniformLocation(program, param.first);
        if (loc >= 0) {
            glUniform1f(loc, param.second);
        }
    }
}

bool ShaderEngine::loadTextureReference(const std::string& name, const std::string& path) {
    // TODO: Implementar carregamento de texturas (PNG, etc.)
    // Por enquanto, apenas log
    LOG_INFO("Textura de referência: " + name + " = " + path);
    return true;
}

void ShaderEngine::cleanupTextureReferences() {
    for (auto& tex : m_textureReferences) {
        if (tex.second != 0) {
            glDeleteTextures(1, &tex.second);
        }
    }
    m_textureReferences.clear();
}

void ShaderEngine::cleanupPresetPasses() {
    for (auto& pass : m_passes) {
        if (pass.program != 0) {
            glDeleteProgram(pass.program);
        }
        if (pass.vertexShader != 0) {
            glDeleteShader(pass.vertexShader);
        }
        if (pass.fragmentShader != 0) {
            glDeleteShader(pass.fragmentShader);
        }
        cleanupFramebuffer(pass.framebuffer, pass.texture);
    }
    m_passes.clear();
}

bool ShaderEngine::compileShader(const std::string& source, GLenum type, GLuint& shader) {
    shader = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        LOG_ERROR("Erro ao compilar shader: " + std::string(infoLog));
        glDeleteShader(shader);
        shader = 0;
        return false;
    }
    
    return true;
}

bool ShaderEngine::linkProgram(GLuint vertexShader, GLuint fragmentShader) {
    m_shaderProgram = glCreateProgram();
    if (m_shaderProgram == 0) {
        LOG_ERROR("Falha ao criar shader program");
        return false;
    }
    
    glAttachShader(m_shaderProgram, vertexShader);
    glAttachShader(m_shaderProgram, fragmentShader);
    glLinkProgram(m_shaderProgram);
    
    GLint success;
    glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(m_shaderProgram, 512, nullptr, infoLog);
        LOG_ERROR("Erro ao linkar shader program: " + std::string(infoLog));
        glDeleteProgram(m_shaderProgram);
        m_shaderProgram = 0;
        return false;
    }
    
    return true;
}

void ShaderEngine::disableShader() {
    if (m_shaderProgram != 0) {
        glDeleteProgram(m_shaderProgram);
        m_shaderProgram = 0;
    }
    if (m_vertexShader != 0) {
        glDeleteShader(m_vertexShader);
        m_vertexShader = 0;
    }
    if (m_fragmentShader != 0) {
        glDeleteShader(m_fragmentShader);
        m_fragmentShader = 0;
    }
    cleanupFramebuffer(m_framebuffer, m_outputTexture);
    m_shaderActive = false;
    m_uniformLocations.clear();
}

GLint ShaderEngine::getUniformLocation(GLuint program, const std::string& name) {
    std::string key = std::to_string(program) + "_" + name;
    auto it = m_uniformLocations.find(key);
    if (it != m_uniformLocations.end()) {
        return it->second;
    }
    
    GLint location = glGetUniformLocation(program, name.c_str());
    if (location >= 0) {
        m_uniformLocations[key] = location;
    }
    return location;
}

void ShaderEngine::createFramebuffer(uint32_t width, uint32_t height, bool floatBuffer, GLuint& fb, GLuint& tex) {
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    
    // Usar RGBA para garantir compatibilidade
    GLenum internalFormat = floatBuffer ? GL_RGBA32F : GL_RGBA;
    GLenum format = GL_RGBA;
    GLenum type = floatBuffer ? GL_FLOAT : GL_UNSIGNED_BYTE;
    
    
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOG_ERROR("Framebuffer incompleto! Status: " + std::to_string(status));
        cleanupFramebuffer(fb, tex);
        return;
    }
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void ShaderEngine::cleanupFramebuffer(GLuint& fb, GLuint& tex) {
    if (tex != 0) {
        glDeleteTextures(1, &tex);
        tex = 0;
    }
    if (fb != 0) {
        glDeleteFramebuffers(1, &fb);
        fb = 0;
    }
}

void ShaderEngine::createQuad() {
    // Quad em coordenadas de clip space (vec4 Position: x, y, z, w)
    // Position é vec4 para compatibilidade com RetroArch
    // IMPORTANTE: Inverter coordenadas Y da textura porque a textura da câmera está invertida
    float vertices[] = {
        // Position (vec4: x, y, z, w) + TexCoord (vec2)
        -1.0f, -1.0f,  0.0f, 1.0f,  0.0f, 1.0f,  // bottom-left (Y invertido: 0.0 -> 1.0)
         1.0f, -1.0f,  0.0f, 1.0f,  1.0f, 1.0f,  // bottom-right (Y invertido: 0.0 -> 1.0)
         1.0f,  1.0f,  0.0f, 1.0f,  1.0f, 0.0f,  // top-right (Y invertido: 1.0 -> 0.0)
        -1.0f,  1.0f,  0.0f, 1.0f,  0.0f, 0.0f   // top-left (Y invertido: 1.0 -> 0.0)
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
    
    // Atributo 0: Position (vec4: x, y, z, w)
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    
    // Atributo 1: TexCoord (vec2)
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(4 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    // Ligar atributos aos nomes do shader (necessário quando não usamos layout(location))
    // Isso será feito ao linkar o programa, mas precisamos garantir que os nomes estejam corretos
    
    glBindVertexArray(0);
}

void ShaderEngine::cleanupQuad() {
    if (m_VAO != 0) {
        glDeleteVertexArrays(1, &m_VAO);
        m_VAO = 0;
    }
    if (m_VBO != 0) {
        glDeleteBuffers(1, &m_VBO);
        m_VBO = 0;
    }
    if (m_EBO != 0) {
        glDeleteBuffers(1, &m_EBO);
        m_EBO = 0;
    }
}

void ShaderEngine::setUniform(const std::string& name, float value) {
    // Implementação para modo simples
    if (m_shaderActive && m_shaderProgram != 0) {
        GLint loc = getUniformLocation(m_shaderProgram, name);
        if (loc >= 0) {
            glUniform1f(loc, value);
        }
    }
}

void ShaderEngine::setUniform(const std::string& name, float x, float y) {
    if (m_shaderActive && m_shaderProgram != 0) {
        GLint loc = getUniformLocation(m_shaderProgram, name);
        if (loc >= 0) {
            glUniform2f(loc, x, y);
        }
    }
}

void ShaderEngine::setUniform(const std::string& name, float x, float y, float z, float w) {
    if (m_shaderActive && m_shaderProgram != 0) {
        GLint loc = getUniformLocation(m_shaderProgram, name);
        if (loc >= 0) {
            glUniform4f(loc, x, y, z, w);
        }
    }
}

std::string ShaderEngine::convertSlangToGLSL(const std::string& slangSource, bool isVertex, const std::string& basePath) {
    std::string result = slangSource;
    
    // Processar #include primeiro (antes de outras conversões)
    result = processIncludes(result, basePath);
    
    // Substituir #version 450 por #version 330
    result = std::regex_replace(result, std::regex("#version\\s+450"), "#version 330");
    
    // Converter push_constant uniform block para uniforms individuais
    // layout(push_constant) uniform Push { vec4 SourceSize; vec4 OriginalSize; ... } params;
    // vira: uniform vec4 SourceSize; uniform vec4 OriginalSize; ...
    // E substituir params.SourceSize por SourceSize, etc.
    
    // Primeiro, extrair campos do uniform block e criar uniforms individuais
    std::regex pushConstantRegex(R"(layout\s*\(\s*push_constant\s*\)\s*uniform\s+Push\s*\{([^}]+)\}\s*params\s*;)");
    std::smatch match;
    
    if (std::regex_search(result, match, pushConstantRegex)) {
        std::string blockContent = match[1].str();
        std::ostringstream uniforms;
        
        // Extrair cada campo do block
        std::regex fieldRegex(R"(\s*(\w+)\s+(\w+)\s*;)");
        std::sregex_iterator iter(blockContent.begin(), blockContent.end(), fieldRegex);
        std::sregex_iterator end;
        
        for (; iter != end; ++iter) {
            std::string type = (*iter)[1].str();
            std::string name = (*iter)[2].str();
            uniforms << "uniform " << type << " " << name << ";\n";
        }
        
        // Substituir todos os params.X por X (fazer isso globalmente após criar os uniforms)
        // Isso garante que funcione também em arquivos incluídos
        std::regex fieldRegex2(R"(\s*(\w+)\s+(\w+)\s*;)");
        std::sregex_iterator iter2(blockContent.begin(), blockContent.end(), fieldRegex2);
        std::sregex_iterator end2;
        
        for (; iter2 != end2; ++iter2) {
            std::string name = (*iter2)[2].str();
            // Substituir params.name por name (globalmente, incluindo em arquivos incluídos)
            // Usar word boundary para evitar substituir partes de outras palavras
            std::string replacePattern = "params\\." + name + "\\b";
            result = std::regex_replace(result, std::regex(replacePattern), name);
        }
        
        // Remover o uniform block original e adicionar uniforms individuais
        result = std::regex_replace(result, pushConstantRegex, uniforms.str());
    } else {
        // Fallback: apenas remover push_constant
        result = std::regex_replace(result, std::regex("layout\\(push_constant\\)\\s+uniform"), "uniform");
    }
    
    // Remover set = X, binding = X dos layouts
    result = std::regex_replace(result, std::regex("set\\s*=\\s*\\d+"), "");
    result = std::regex_replace(result, std::regex("binding\\s*=\\s*\\d+"), "");
    result = std::regex_replace(result, std::regex(",\\s*,"), ",");
    result = std::regex_replace(result, std::regex("layout\\(\\s*,\\s*"), "layout(");
    result = std::regex_replace(result, std::regex("layout\\(\\s*\\)"), "");
    
    // Remover layout(std140, set = 0, binding = 0) deixando apenas std140 se existir
    result = std::regex_replace(result, std::regex("layout\\(std140[^)]*\\)"), "layout(std140)");
    
    // Remover uniform block UBO (se existir)
    // layout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; } global;
    std::regex uboRegex(R"(layout\s*\([^)]*\)\s*uniform\s+UBO\s*\{[^}]*mat4\s+MVP[^}]*\}\s*global\s*;)");
    result = std::regex_replace(result, uboRegex, "");
    
    // Converter global.MVP para Position diretamente
    // RetroArch usa global.MVP * Position, mas Position já está em clip space
    result = std::regex_replace(result, std::regex(R"(global\.MVP\s*\*\s*Position)"), "Position");
    
    // Remover outras referências a global.
    result = std::regex_replace(result, std::regex(R"(global\.)"), "");
    
    // IMPORTANTE: Substituir params.X por X DEPOIS de todas as conversões
    // Isso garante que funcione mesmo em arquivos incluídos
    
    // PRIMEIRO: Processar #define que usam params.X
    // Exemplo: #define BLURSCALEX params.BLURSCALEX
    // Coletar todos os parâmetros customizados que precisam ser uniforms
    std::set<std::string> customParams;
    std::regex defineRegex(R"(#define\s+(\w+)\s+params\.(\w+))");
    std::smatch defineMatch;
    std::string processedDefines = result;
    
    // Encontrar todos os #define que usam params.X
    while (std::regex_search(processedDefines, defineMatch, defineRegex)) {
        std::string defineName = defineMatch[1].str();
        std::string paramName = defineMatch[2].str();
        
        // Adicionar à lista de parâmetros customizados
        customParams.insert(paramName);
        
        // Se o nome do define é igual ao parâmetro, remover o define
        // (não é necessário, pois params.X será substituído por X diretamente)
        if (defineName == paramName) {
            processedDefines = std::regex_replace(processedDefines, defineRegex, "", std::regex_constants::format_first_only);
        } else {
            // Se são diferentes, manter o define mas substituir params.X por X
            std::string replacement = "#define " + defineName + " " + paramName;
            processedDefines = std::regex_replace(processedDefines, defineRegex, replacement, std::regex_constants::format_first_only);
        }
    }
    result = processedDefines;
    
    // AGORA: Substituir TODOS os params.X por X (genérico, pega tudo)
    // Isso deve ser feito DEPOIS de processar os #define
    result = std::regex_replace(result, std::regex(R"(params\.(\w+))"), "$1");
    
    // Agora verificar se os uniforms já foram definidos
    bool hasSourceSize = result.find("uniform vec4 SourceSize") != std::string::npos;
    bool hasOriginalSize = result.find("uniform vec4 OriginalSize") != std::string::npos;
    bool hasOutputSize = result.find("uniform vec4 OutputSize") != std::string::npos;
    bool hasFrameCount = result.find("uniform") != std::string::npos && result.find("FrameCount") != std::string::npos;
    
    // Se não foram definidos mas são usados, adicionar as definições
    std::ostringstream missingUniforms;
    if (!hasSourceSize && result.find("SourceSize") != std::string::npos) {
        missingUniforms << "uniform vec4 SourceSize;\n";
    }
    if (!hasOriginalSize && result.find("OriginalSize") != std::string::npos) {
        missingUniforms << "uniform vec4 OriginalSize;\n";
    }
    if (!hasOutputSize && result.find("OutputSize") != std::string::npos) {
        missingUniforms << "uniform vec4 OutputSize;\n";
    }
    if (!hasFrameCount && result.find("FrameCount") != std::string::npos) {
        missingUniforms << "uniform float FrameCount;\n";
    }
    
    // Adicionar uniforms customizados encontrados nos #define
    // Assumir que são floats (padrão comum no RetroArch)
    for (const auto& param : customParams) {
        // Verificar se já não foi definido
        std::string uniformDecl = "uniform float " + param;
        if (result.find(uniformDecl) == std::string::npos && result.find(param) != std::string::npos) {
            missingUniforms << uniformDecl << ";\n";
        }
    }
    
    // Inserir uniforms faltantes após a versão
    if (!missingUniforms.str().empty()) {
        std::regex versionRegex(R"(#version\s+\d+)");
        result = std::regex_replace(result, versionRegex, "$&\n" + missingUniforms.str(), std::regex_constants::format_first_only);
    }
    
    // Separar vertex e fragment shaders usando #pragma stage
    if (!isVertex) {
        // Fragment shader: remover seções vertex, manter apenas fragment
        std::istringstream stream(result);
        std::string line;
        std::ostringstream fragmentOutput;
        bool inVertexStage = false;
        bool inFragmentStage = false;
        bool hasPragma = false;
        
        while (std::getline(stream, line)) {
            if (line.find("#pragma stage vertex") != std::string::npos) {
                inVertexStage = true;
                inFragmentStage = false;
                hasPragma = true;
                continue;
            }
            if (line.find("#pragma stage fragment") != std::string::npos) {
                inVertexStage = false;
                inFragmentStage = true;
                hasPragma = true;
                continue;
            }
            if (line.find("#pragma") != std::string::npos && line.find("stage") == std::string::npos) {
                // Outro pragma, manter
                fragmentOutput << line << "\n";
                continue;
            }
            
            if (hasPragma) {
                if (inFragmentStage || (!inVertexStage && !inFragmentStage)) {
                    fragmentOutput << line << "\n";
                }
            } else {
                // Sem pragma, assumir que é fragment shader
                fragmentOutput << line << "\n";
            }
        }
        
        if (hasPragma) {
            result = fragmentOutput.str();
        }
    } else {
        // Vertex shader: remover seções fragment, manter apenas vertex
        std::istringstream stream(result);
        std::string line;
        std::ostringstream vertexOutput;
        bool inVertexStage = false;
        bool inFragmentStage = false;
        bool hasPragma = false;
        
        while (std::getline(stream, line)) {
            if (line.find("#pragma stage vertex") != std::string::npos) {
                inVertexStage = true;
                inFragmentStage = false;
                hasPragma = true;
                continue;
            }
            if (line.find("#pragma stage fragment") != std::string::npos) {
                inVertexStage = false;
                inFragmentStage = true;
                hasPragma = true;
                continue;
            }
            if (line.find("#pragma") != std::string::npos && line.find("stage") == std::string::npos) {
                vertexOutput << line << "\n";
                continue;
            }
            
            if (hasPragma) {
                if (inVertexStage || (!inVertexStage && !inFragmentStage)) {
                    vertexOutput << line << "\n";
                }
            } else {
                // Sem pragma, assumir que é vertex shader
                vertexOutput << line << "\n";
            }
        }
        
        if (hasPragma) {
            result = vertexOutput.str();
        }
    }
    
    // Remover pragmas de stage após processamento
    result = std::regex_replace(result, std::regex("#pragma\\s+stage\\s+(vertex|fragment)\\s*"), "");
    
    // Remover layout(location = X) - não suportado em GLSL 3.30 sem extensões
    // layout(location = 0) in/out -> in/out
    result = std::regex_replace(result, std::regex("layout\\(location\\s*=\\s*\\d+\\)\\s+"), "");
    
    // Converter uint para float (OpenGL 3.3 não suporta uint uniforms de forma confiável)
    result = std::regex_replace(result, std::regex("uniform\\s+uint\\s+"), "uniform float ");
    
    // Limpar linhas vazias múltiplas
    result = std::regex_replace(result, std::regex("\n\\s*\n\\s*\n"), "\n\n");
    
    return result;
}

std::string ShaderEngine::processIncludes(const std::string& source, const std::string& basePath) {
    std::string result = source;
    std::regex includeRegex(R"(#include\s+["<]([^">]+)[">])");
    std::smatch match;
    
    // Processar todos os includes
    while (std::regex_search(result, match, includeRegex)) {
        std::string includePath = match[1].str();
        std::string fullPath;
        
        // Tentar resolver o caminho
        if (includePath[0] == '/') {
            // Caminho absoluto
            fullPath = includePath;
        } else {
            // Caminho relativo - tentar várias localizações
            std::filesystem::path currentPath = std::filesystem::current_path();
            
            // 1. Relativo ao diretório do shader atual
            if (!basePath.empty()) {
                std::filesystem::path base(basePath);
                std::filesystem::path resolved = base / includePath;
                if (std::filesystem::exists(resolved)) {
                    fullPath = resolved.string();
                }
            }
            
            // 2. Em shaders/shaders_slang/
            if (fullPath.empty()) {
                std::filesystem::path slangPath = currentPath / "shaders" / "shaders_slang" / includePath;
                if (std::filesystem::exists(slangPath)) {
                    fullPath = slangPath.string();
                }
            }
            
            // 3. Relativo ao diretório atual
            if (fullPath.empty()) {
                std::filesystem::path relPath = currentPath / includePath;
                if (std::filesystem::exists(relPath)) {
                    fullPath = relPath.string();
                }
            }
            
            // 4. Tentar com caminho relativo do shader (subindo diretórios)
            if (fullPath.empty() && !basePath.empty()) {
                std::filesystem::path base(basePath);
                // Remover "../" do início
                std::string cleanPath = includePath;
                while (cleanPath.find("../") == 0) {
                    cleanPath = cleanPath.substr(3);
                    base = base.parent_path();
                }
                std::filesystem::path resolved = base / cleanPath;
                if (std::filesystem::exists(resolved)) {
                    fullPath = resolved.string();
                }
            }
        }
        
        if (!fullPath.empty() && std::filesystem::exists(fullPath)) {
            // Carregar arquivo incluído
            std::ifstream includeFile(fullPath);
            if (includeFile.is_open()) {
                std::stringstream includeBuffer;
                includeBuffer << includeFile.rdbuf();
                std::string includeContent = includeBuffer.str();
                includeFile.close();
                
                // Processar includes recursivamente no arquivo incluído
                std::filesystem::path includeFilePath(fullPath);
                std::string includeDir = includeFilePath.parent_path().string();
                includeContent = processIncludes(includeContent, includeDir);
                
                // Substituir o #include pelo conteúdo
                result = std::regex_replace(result, includeRegex, includeContent, std::regex_constants::format_first_only);
                LOG_INFO("Arquivo incluído: " + fullPath);
            } else {
                LOG_WARN("Falha ao abrir arquivo incluído: " + fullPath);
                // Remover o #include que falhou
                result = std::regex_replace(result, includeRegex, "", std::regex_constants::format_first_only);
            }
        } else {
            LOG_WARN("Arquivo incluído não encontrado: " + includePath + " (base: " + basePath + ")");
            // Remover o #include que falhou
            result = std::regex_replace(result, includeRegex, "", std::regex_constants::format_first_only);
        }
    }
    
    return result;
}

