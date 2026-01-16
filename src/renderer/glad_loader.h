#pragma once

#include <cstddef>
#include <string>

// Header simples para carregar funções OpenGL via GLFW
// GLFW pode carregar as funções OpenGL diretamente

#ifdef __cplusplus
extern "C" {
#endif

// Declarações das funções OpenGL que precisamos
// Essas serão carregadas dinamicamente via GLFW

typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef signed char GLbyte;
typedef short GLshort;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef void GLvoid;
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;

// Funções OpenGL 3.3 Core
extern GLuint (*glCreateShader)(GLenum type);
extern void (*glShaderSource)(GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length);
extern void (*glCompileShader)(GLuint shader);
extern void (*glGetShaderiv)(GLuint shader, GLenum pname, GLint* params);
extern void (*glGetShaderInfoLog)(GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
extern void (*glDeleteShader)(GLuint shader);
extern GLuint (*glCreateProgram)(void);
extern void (*glAttachShader)(GLuint program, GLuint shader);
extern void (*glLinkProgram)(GLuint program);
extern void (*glGetProgramiv)(GLuint program, GLenum pname, GLint* params);
extern void (*glGetProgramInfoLog)(GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog);
extern void (*glDeleteProgram)(GLuint program);
extern void (*glUseProgram)(GLuint program);
extern void (*glGenVertexArrays)(GLsizei n, GLuint* arrays);
extern void (*glDeleteVertexArrays)(GLsizei n, const GLuint* arrays);
extern void (*glBindVertexArray)(GLuint array);
extern void (*glGenBuffers)(GLsizei n, GLuint* buffers);
extern void (*glDeleteBuffers)(GLsizei n, const GLuint* buffers);
extern void (*glBindBuffer)(GLenum target, GLuint buffer);
extern void (*glBufferData)(GLenum target, GLsizeiptr size, const void* data, GLenum usage);
extern void* (*glMapBuffer)(GLenum target, GLenum access);
extern GLboolean (*glUnmapBuffer)(GLenum target);
extern void (*glVertexAttribPointer)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer);
extern void (*glEnableVertexAttribArray)(GLuint index);
extern void (*glGenTextures)(GLsizei n, GLuint* textures);
extern void (*glDeleteTextures)(GLsizei n, const GLuint* textures);
extern void (*glBindTexture)(GLenum target, GLuint texture);
extern void (*glTexParameteri)(GLenum target, GLenum pname, GLint param);
extern void (*glTexImage2D)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* data);
extern void (*glTexSubImage2D)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* data);
extern void (*glGenFramebuffers)(GLsizei n, GLuint* framebuffers);
extern void (*glDeleteFramebuffers)(GLsizei n, const GLuint* framebuffers);
extern void (*glBindFramebuffer)(GLenum target, GLuint framebuffer);
extern void (*glFramebufferTexture2D)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
extern GLenum (*glCheckFramebufferStatus)(GLenum target);
extern void (*glGenerateMipmap)(GLenum target);
extern void (*glBlitFramebuffer)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
extern void (*glActiveTexture)(GLenum texture);
extern GLint (*glGetUniformLocation)(GLuint program, const char* name);
extern void (*glBindAttribLocation)(GLuint program, GLuint index, const char* name);
extern void (*glGetActiveUniform)(GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLint* size, GLenum* type, GLchar* name);
extern void (*glUniform1i)(GLint location, GLint v0);
extern void (*glUniform1f)(GLint location, GLfloat v0);
extern void (*glUniform2f)(GLint location, GLfloat v0, GLfloat v1);
extern void (*glUniform3f)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
extern void (*glUniform4f)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
extern void (*glUniformMatrix4fv)(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value);

// Funções básicas do OpenGL 1.x/2.x - usamos as versões estáticas linkadas
// Declarações forward (implementações vêm do OpenGL linkado estaticamente)
#ifdef __cplusplus
extern "C" {
#endif
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glClear(GLbitfield mask);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
void glEnable(GLenum cap);
void glDisable(GLenum cap);
void glBlendFunc(GLenum sfactor, GLenum dfactor);
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
void glFinish(void);
void glFlush(void);
#ifdef __cplusplus
}
#endif

// glGetIntegerv - pode precisar ser carregado dinamicamente em alguns contextos
extern void (*glGetIntegerv)(GLenum pname, GLint* params);

// Constantes OpenGL
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_LINEAR 0x2601
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_INT 0x1404
#define GL_FLOAT 0x1406
#define GL_RGBA32F 0x8814
#define GL_FALSE 0
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_TRIANGLES 0x0004
#define GL_UNSIGNED_INT 0x1405
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_TEXTURE0 0x84C0
#define GL_RGBA32F 0x8814
#define GL_RGBA 0x1908
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_BORDER 0x812D
#define GL_MIRRORED_REPEAT 0x8370
#define GL_NEAREST 0x2600
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_CULL_FACE 0x0B44

// PBO (Pixel Buffer Object) constants
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_STREAM_READ 0x88E1
#define GL_READ_ONLY 0x88B8
#define GL_DEPTH_TEST 0x0B71
#define GL_ACTIVE_UNIFORMS 0x8B86
#define GL_FLOAT_VEC2 0x8B50
#define GL_FLOAT_VEC3 0x8B51
#define GL_FLOAT_VEC4 0x8B52
#define GL_TRUE 1
#define GL_FALSE 0
#define GL_VIEWPORT 0x0BA2
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_VERSION 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_MAJOR_VERSION 0x821B

// Funções básicas do OpenGL que podem estar disponíveis estaticamente
// glGetString está disponível desde OpenGL 1.0, então pode ser linkado estaticamente
#ifdef __cplusplus
extern "C" {
#endif
const GLubyte* glGetString(GLenum name);
#ifdef __cplusplus
}
#endif

bool loadOpenGLFunctions();

// Funções para detectar versão OpenGL e GLSL
// Retorna a versão GLSL apropriada baseada na versão OpenGL disponível
std::string getGLSLVersionString();
// Retorna true se OpenGL ES está sendo usado
bool isOpenGLES();
// Retorna a versão major do OpenGL (3, 2, etc.)
int getOpenGLMajorVersion();

#ifdef __cplusplus
}
#endif

