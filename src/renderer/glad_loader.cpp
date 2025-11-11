#include "glad_loader.h"
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <cstring>
#include "../utils/Logger.h"

// Definir os ponteiros de função
GLuint (*glCreateShader)(GLenum) = nullptr;
void (*glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
void (*glCompileShader)(GLuint) = nullptr;
void (*glGetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
void (*glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
void (*glDeleteShader)(GLuint) = nullptr;
GLuint (*glCreateProgram)(void) = nullptr;
void (*glAttachShader)(GLuint, GLuint) = nullptr;
void (*glLinkProgram)(GLuint) = nullptr;
void (*glGetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
void (*glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
void (*glDeleteProgram)(GLuint) = nullptr;
void (*glUseProgram)(GLuint) = nullptr;
void (*glGenVertexArrays)(GLsizei, GLuint*) = nullptr;
void (*glDeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
void (*glBindVertexArray)(GLuint) = nullptr;
void (*glGenBuffers)(GLsizei, GLuint*) = nullptr;
void (*glDeleteBuffers)(GLsizei, const GLuint*) = nullptr;
void (*glBindBuffer)(GLenum, GLuint) = nullptr;
void (*glBufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
void (*glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
void (*glEnableVertexAttribArray)(GLuint) = nullptr;
void (*glGenTextures)(GLsizei, GLuint*) = nullptr;
void (*glDeleteTextures)(GLsizei, const GLuint*) = nullptr;
void (*glBindTexture)(GLenum, GLuint) = nullptr;
void (*glTexParameteri)(GLenum, GLenum, GLint) = nullptr;
void (*glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
void (*glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*) = nullptr;
void (*glGenFramebuffers)(GLsizei, GLuint*) = nullptr;
void (*glDeleteFramebuffers)(GLsizei, const GLuint*) = nullptr;
void (*glBindFramebuffer)(GLenum, GLuint) = nullptr;
void (*glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
GLenum (*glCheckFramebufferStatus)(GLenum) = nullptr;
void (*glActiveTexture)(GLenum) = nullptr;
GLint (*glGetUniformLocation)(GLuint, const char*) = nullptr;
void (*glBindAttribLocation)(GLuint, GLuint, const char*) = nullptr;
void (*glUniform1i)(GLint, GLint) = nullptr;
void (*glUniform1f)(GLint, GLfloat) = nullptr;
void (*glUniform2f)(GLint, GLfloat, GLfloat) = nullptr;
void (*glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;

// Funções básicas (glViewport, glClearColor, glClear, glDrawElements) são do OpenGL 1.x/2.x
// e estão linkadas estaticamente via OpenGL::GL - não precisam ser declaradas aqui

bool loadOpenGLFunctions() {
    // Carregar funções via glfwGetProcAddress (forma recomendada)
    // Isso funciona tanto com OpenGL quanto com OpenGL ES
    
    #define LOAD_FUNC(name) \
        name = reinterpret_cast<decltype(name)>(glfwGetProcAddress(#name)); \
        if (!name) { \
            LOG_ERROR("Falha ao carregar função: " #name); \
            return false; \
        }
    
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
    LOAD_FUNC(glActiveTexture)
    LOAD_FUNC(glGetUniformLocation)
    LOAD_FUNC(glBindAttribLocation)
    LOAD_FUNC(glUniform1i)
    LOAD_FUNC(glUniform1f)
    LOAD_FUNC(glUniform2f)
    LOAD_FUNC(glUniform4f)
    
    // Funções básicas (glViewport, glClearColor, glClear, glDrawElements) são do OpenGL 1.x/2.x
    // e estão linkadas estaticamente - não precisam ser carregadas dinamicamente
    
    #undef LOAD_FUNC
    
    LOG_INFO("Funções OpenGL carregadas com sucesso");
    return true;
}

