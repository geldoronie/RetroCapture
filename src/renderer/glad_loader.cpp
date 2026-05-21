#include "glad_loader.h"
#include <cstring>
#include <string>
#include "../utils/Logger.h"

#ifdef USE_SDL2
// SDL2: Apenas incluir SDL.h para SDL_GL_GetProcAddress
// NÃO incluir SDL_opengl.h pois ele declara as funções OpenGL como funções normais
// e conflita com nossas declarações de ponteiros de função
#include <SDL2/SDL.h>
#else
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#endif

// Definir os ponteiros de função
GLuint (*glCreateShader)(GLenum) = nullptr;
void (*glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *) = nullptr;
void (*glCompileShader)(GLuint) = nullptr;
void (*glGetShaderiv)(GLuint, GLenum, GLint *) = nullptr;
void (*glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *) = nullptr;
void (*glDeleteShader)(GLuint) = nullptr;
GLuint (*glCreateProgram)(void) = nullptr;
void (*glAttachShader)(GLuint, GLuint) = nullptr;
void (*glLinkProgram)(GLuint) = nullptr;
void (*glGetProgramiv)(GLuint, GLenum, GLint *) = nullptr;
void (*glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *) = nullptr;
void (*glDeleteProgram)(GLuint) = nullptr;
void (*glUseProgram)(GLuint) = nullptr;
void (*glGenVertexArrays)(GLsizei, GLuint *) = nullptr;
void (*glDeleteVertexArrays)(GLsizei, const GLuint *) = nullptr;
void (*glBindVertexArray)(GLuint) = nullptr;
void (*glGenBuffers)(GLsizei, GLuint *) = nullptr;
void (*glDeleteBuffers)(GLsizei, const GLuint *) = nullptr;
void (*glBindBuffer)(GLenum, GLuint) = nullptr;
void (*glBufferData)(GLenum, GLsizeiptr, const void *, GLenum) = nullptr;
void* (*glMapBuffer)(GLenum, GLenum) = nullptr;
GLboolean (*glUnmapBuffer)(GLenum) = nullptr;
void (*glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *) = nullptr;
void (*glEnableVertexAttribArray)(GLuint) = nullptr;
void (*glGenTextures)(GLsizei, GLuint *) = nullptr;
void (*glDeleteTextures)(GLsizei, const GLuint *) = nullptr;
void (*glBindTexture)(GLenum, GLuint) = nullptr;
void (*glTexParameteri)(GLenum, GLenum, GLint) = nullptr;
void (*glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *) = nullptr;
void (*glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *) = nullptr;
void (*glGenFramebuffers)(GLsizei, GLuint *) = nullptr;
void (*glDeleteFramebuffers)(GLsizei, const GLuint *) = nullptr;
void (*glBindFramebuffer)(GLenum, GLuint) = nullptr;
void (*glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
GLenum (*glCheckFramebufferStatus)(GLenum) = nullptr;
void (*glGenerateMipmap)(GLenum) = nullptr;
void (*glBlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum) = nullptr;
void (*glActiveTexture)(GLenum) = nullptr;
GLint (*glGetUniformLocation)(GLuint, const char *) = nullptr;
void (*glBindAttribLocation)(GLuint, GLuint, const char *) = nullptr;
void (*glGetActiveUniform)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *) = nullptr;
void (*glUniform1i)(GLint, GLint) = nullptr;
void (*glUniform1f)(GLint, GLfloat) = nullptr;
void (*glUniform2f)(GLint, GLfloat, GLfloat) = nullptr;
void (*glUniform3f)(GLint, GLfloat, GLfloat, GLfloat) = nullptr;
void (*glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
void (*glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *) = nullptr;
void (*glGetIntegerv)(GLenum, GLint*) = nullptr;

// Funções básicas (glViewport, glClearColor, glClear, glDrawElements) são do OpenGL 1.x/2.x
// e estão linkadas estaticamente via OpenGL::GL - não precisam ser declaradas aqui

bool loadOpenGLFunctions()
{
#ifdef USE_SDL2
    // SDL2: Verificar se SDL foi inicializado
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0)
    {
        LOG_ERROR("SDL2 não foi inicializado - não é possível carregar funções OpenGL");
        return false;
    }

    // SDL2: Usar SDL_GL_GetProcAddress para carregar funções OpenGL
#define LOAD_FUNC(name)                                                 \
    name = reinterpret_cast<decltype(name)>(SDL_GL_GetProcAddress(#name)); \
    if (!name)                                                          \
    {                                                                   \
        LOG_ERROR("Falha ao carregar função OpenGL: " #name);           \
        return false;                                                   \
    }
#else
    // GLFW: Carregar funções via glfwGetProcAddress (forma recomendada)
    // Isso funciona tanto com OpenGL quanto com OpenGL ES

    // Verificar se o contexto OpenGL está ativo
    if (!glfwGetCurrentContext())
    {
        LOG_ERROR("Contexto OpenGL não está ativo - não é possível carregar funções OpenGL");
        return false;
    }

#define LOAD_FUNC(name)                                                 \
    name = reinterpret_cast<decltype(name)>(glfwGetProcAddress(#name)); \
    if (!name)                                                          \
    {                                                                   \
        LOG_ERROR("Falha ao carregar função OpenGL: " #name);           \
        return false;                                                   \
    }
#endif

    // Funções OpenGL 3.3+ Core (precisam ser carregadas dinamicamente)
    LOAD_FUNC(glCreateShader)
    LOAD_FUNC(glShaderSource)
    LOAD_FUNC(glCompileShader)
    LOAD_FUNC(glGetShaderiv)
    LOAD_FUNC(glGetShaderInfoLog)
    LOAD_FUNC(glDeleteShader)
    LOAD_FUNC(glCreateProgram)
    LOAD_FUNC(glAttachShader)
    LOAD_FUNC(glLinkProgram)
    LOAD_FUNC(glGetProgramiv)
    LOAD_FUNC(glGetProgramInfoLog)
    LOAD_FUNC(glDeleteProgram)
    LOAD_FUNC(glUseProgram)
    LOAD_FUNC(glGenVertexArrays)
    LOAD_FUNC(glDeleteVertexArrays)
    LOAD_FUNC(glBindVertexArray)
    LOAD_FUNC(glGenBuffers)
    LOAD_FUNC(glDeleteBuffers)
    LOAD_FUNC(glBindBuffer)
    LOAD_FUNC(glBufferData)
    LOAD_FUNC(glMapBuffer)
    LOAD_FUNC(glUnmapBuffer)
    LOAD_FUNC(glVertexAttribPointer)
    LOAD_FUNC(glEnableVertexAttribArray)

    // Funções de textura (algumas podem estar disponíveis estaticamente, mas carregamos dinamicamente para garantir)
    LOAD_FUNC(glGenTextures)
    LOAD_FUNC(glDeleteTextures)
    LOAD_FUNC(glBindTexture)
    LOAD_FUNC(glTexParameteri)
    LOAD_FUNC(glTexImage2D)
    LOAD_FUNC(glTexSubImage2D)

    // Funções de framebuffer
    LOAD_FUNC(glGenFramebuffers)
    LOAD_FUNC(glDeleteFramebuffers)
    LOAD_FUNC(glBindFramebuffer)
    LOAD_FUNC(glFramebufferTexture2D)
    LOAD_FUNC(glCheckFramebufferStatus)
    LOAD_FUNC(glGenerateMipmap)
    LOAD_FUNC(glBlitFramebuffer)
    LOAD_FUNC(glActiveTexture)
    LOAD_FUNC(glGetUniformLocation)
    LOAD_FUNC(glBindAttribLocation)
    LOAD_FUNC(glGetActiveUniform)
    LOAD_FUNC(glUniform1i)
    LOAD_FUNC(glUniform1f)
    LOAD_FUNC(glUniform2f)
    LOAD_FUNC(glUniform3f)
    LOAD_FUNC(glUniform4f)
    LOAD_FUNC(glUniformMatrix4fv)
    
    // glGetIntegerv - carregar dinamicamente para garantir compatibilidade
    LOAD_FUNC(glGetIntegerv)

    // glEnable, glDisable, glBlendFunc são funções do OpenGL 1.x/2.x
    // e estão disponíveis estaticamente - não precisam ser carregadas dinamicamente

    // Funções básicas (glViewport, glClearColor, glClear, glDrawElements) são do OpenGL 1.x/2.x
    // e estão linkadas estaticamente - não precisam ser carregadas dinamicamente

#undef LOAD_FUNC

    // Verificar se as funções críticas foram carregadas
    if (!glGenVertexArrays || !glGenBuffers || !glCreateShader || !glCreateProgram)
    {
        LOG_ERROR("Funções OpenGL críticas não foram carregadas");
        return false;
    }

    LOG_INFO("Funções OpenGL carregadas com sucesso");
    return true;
}

// Funções para detectar versão OpenGL
bool isOpenGLES()
{
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (!version) return false;
    
    std::string versionStr(version);
    
    // Verificar se contém "OpenGL ES" explicitamente
    if (versionStr.find("OpenGL ES") != std::string::npos) {
        return true;
    }
    
    // Verificar outras pistas de OpenGL ES:
    // - Mesa em sistemas ARM geralmente usa OpenGL ES quando via framebuffer
    // - Verificar se contém "Mesa" e estamos em ARM
    #if defined(__arm__) || defined(__aarch64__) || defined(ARCH_ARM) || defined(ARCH_ARM32) || defined(ARCH_ARM64)
        if (versionStr.find("Mesa") != std::string::npos) {
            // Em ARM com Mesa via framebuffer, geralmente é OpenGL ES
            // Verificar se não é explicitamente Desktop OpenGL
            // Se não contém "OpenGL" ou contém "OpenGL ES", é ES
            if (versionStr.find("OpenGL") == std::string::npos) {
                // String não menciona "OpenGL" explicitamente - provavelmente ES
                return true;
            }
            // Se menciona "OpenGL" mas não "OpenGL ES", verificar contexto
            // Em ARM com Mesa via framebuffer, mesmo contexto 2.1 pode ser ES
            const char* video_driver = std::getenv("SDL_VIDEODRIVER");
            if (video_driver && (std::string(video_driver) == "fbcon" || std::string(video_driver) == "directfb")) {
                // Usando framebuffer ou DirectFB em ARM - provavelmente ES
                return true;
            }
        }
        
        // Em ARM, se estamos usando framebuffer/DirectFB, assumir ES mesmo que
        // a string de versão não diga explicitamente
        const char* video_driver = std::getenv("SDL_VIDEODRIVER");
        if (video_driver && (std::string(video_driver) == "fbcon" || std::string(video_driver) == "directfb")) {
            // Verificar se não é explicitamente Desktop
            if (versionStr.find("OpenGL") == std::string::npos || 
                versionStr.find("OpenGL ES") != std::string::npos) {
                return true;
            }
        }
    #endif
    
    return false;
}

int getOpenGLMajorVersion()
{
    // glGetString está disponível estaticamente desde OpenGL 1.0
    
    // Tentar usar GL_MAJOR_VERSION primeiro (OpenGL 3.0+)
    if (glGetIntegerv) {
        GLint major = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        if (major > 0) {
            return static_cast<int>(major);
        }
    }
    
    // Fallback: fazer parsing de GL_VERSION (funciona em todas as versões)
    const char* versionStr = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (!versionStr) return 0;
    
    std::string version(versionStr);
    // Formato típico: "X.Y" ou "X.Y.Z" ou "OpenGL ES X.Y"
    size_t firstDot = version.find('.');
    if (firstDot != std::string::npos) {
        // Encontrar início do número
        size_t start = 0;
        // Pular "OpenGL ES " se presente
        size_t esPos = version.find("OpenGL ES");
        if (esPos != std::string::npos) {
            start = version.find(' ', esPos);
            if (start != std::string::npos) start++;
            else start = esPos + 9; // "OpenGL ES" = 9 chars
        }
        
        try {
            int major = std::stoi(version.substr(start, firstDot - start));
            return major;
        } catch (...) {
            // Ignorar erro de conversão
        }
    }
    
    return 0; // Não foi possível detectar
}

std::string getGLSLVersionString()
{
    // Tentar detectar versão OpenGL
    // glGetString está disponível estaticamente desde OpenGL 1.0
    bool isES = isOpenGLES();
    int major = getOpenGLMajorVersion();
    
    // Se não conseguimos detectar, usar fallback seguro
    if (major == 0) {
        LOG_WARN("Não foi possível detectar versão OpenGL, usando GLSL 1.20 como fallback");
        return "#version 120";
    }
    
    LOG_INFO("OpenGL versão detectada: " + std::to_string(major) + (isES ? " (ES)" : " (Desktop)"));
    
    if (isES) {
        // OpenGL ES
        if (major >= 3) {
            return "#version 300 es";
        } else {
            return "#version 100"; // OpenGL ES 2.0
        }
    } else {
        // OpenGL Desktop
        if (major >= 3) {
            // OpenGL 3.0+ - tentar detectar versão GLSL disponível
            {
                const char* glslVersion = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
                if (glslVersion) {
                    std::string glslStr(glslVersion);
                    // Extrair versão major (formato: "X.Y" ou "X.Y.Z")
                    size_t dotPos = glslStr.find('.');
                    if (dotPos != std::string::npos) {
                        try {
                            int glslMajor = std::stoi(glslStr.substr(0, dotPos));
                            if (glslMajor >= 3) {
                                return "#version 330";
                            } else if (glslMajor >= 1) {
                                return "#version 130";
                            }
                        } catch (...) {
                            // Ignorar erro de conversão
                        }
                    }
                }
            }
            // Fallback baseado na versão OpenGL
            if (major >= 3) {
                return "#version 330";
            }
            return "#version 130";
        } else if (major == 2) {
            // OpenGL 2.1
            LOG_INFO("Usando GLSL 1.20 para OpenGL 2.1");
            return "#version 120";
        } else {
            // OpenGL 1.x
            return "#version 110";
        }
    }
}
