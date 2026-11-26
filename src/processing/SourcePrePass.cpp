#include "SourcePrePass.h"
#include "../renderer/OpenGLRenderer.h"
#include "../utils/Logger.h"
#include <cmath>
#include <string>

#ifndef GL_CURRENT_PROGRAM
#define GL_CURRENT_PROGRAM 0x8B8D
#endif
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x8069
#endif
#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif

namespace
{
std::string trimmedGLSLVersion()
{
    std::string v = getGLSLVersionString();
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' '))
        v.pop_back();
    while (!v.empty() && (v.front() == ' ' || v.front() == '\n' || v.front() == '\r'))
        v.erase(0, 1);
    return v;
}

// Same GLSL dialect selection as OpenGLRenderer's fragment shader; the body
// is shared and written against the SRC_TEX / FRAG_OUT macros.
std::string buildFragmentShader()
{
    const std::string version = trimmedGLSLVersion();
    const bool isES = isOpenGLES();
    const int major = getOpenGLMajorVersion();

    std::string header;
    if (isES && major >= 3)
    {
        header = version + "\n"
                 "precision highp float;\n"
                 "in vec2 TexCoord;\n"
                 "out vec4 FragColor;\n"
                 "#define SRC_TEX texture\n"
                 "#define FRAG_OUT FragColor\n";
    }
    else if (isES)
    {
        header = version + "\n"
                 "precision mediump float;\n"
                 "varying vec2 TexCoord;\n"
                 "#define SRC_TEX texture2D\n"
                 "#define FRAG_OUT gl_FragColor\n";
    }
    else if (major >= 3)
    {
        header = version + " core\n"
                 "in vec2 TexCoord;\n"
                 "out vec4 FragColor;\n"
                 "#define SRC_TEX texture\n"
                 "#define FRAG_OUT FragColor\n";
    }
    else
    {
        header = version + "\n"
                 "varying vec2 TexCoord;\n"
                 "#define SRC_TEX texture2D\n"
                 "#define FRAG_OUT gl_FragColor\n";
    }

    return header +
           "uniform sampler2D uTex;\n"
           "uniform float uExpand;\n"  // number of 16-235 -> 0-255 expansions (0..2)
           "uniform float uSaturation;\n"
           "uniform float uInvGamma;\n"
           "uniform float uDeint;\n"   // 0 none, 1 bob, 2 blend (#229)
           "uniform float uField;\n"   // bob: row parity kept (0 = even/top)
           "uniform float uTexH;\n"
           "\n"
           "vec3 rowSample(float r) {\n"
           "    return SRC_TEX(uTex, vec2(TexCoord.x, (r + 0.5) / uTexH)).rgb;\n"
           "}\n"
           "\n"
           "void main() {\n"
           "    vec3 c;\n"
           "    if (uDeint > 0.5) {\n"
           "        // Row 0 of the texture is the first (top) line of the frame.\n"
           "        float r = floor(TexCoord.y * uTexH);\n"
           "        float up = (r >= 1.0) ? r - 1.0 : r + 1.0;\n"
           "        float dn = (r + 1.0 <= uTexH - 1.0) ? r + 1.0 : r - 1.0;\n"
           "        if (uDeint < 1.5) {\n"
           "            // Bob: keep the chosen field, interpolate the other one.\n"
           "            if (abs(mod(r, 2.0) - uField) < 0.5) c = rowSample(r);\n"
           "            else c = 0.5 * (rowSample(up) + rowSample(dn));\n"
           "        } else {\n"
           "            // Blend: [1 2 1]/4 vertical filter across both fields.\n"
           "            c = 0.5 * rowSample(r) + 0.25 * (rowSample(up) + rowSample(dn));\n"
           "        }\n"
           "    } else {\n"
           "        c = SRC_TEX(uTex, TexCoord).rgb;\n"
           "    }\n"
           "    // Limited (16-235) -> full (0-255) range: once for a limited RGB\n"
           "    // source, once more for 'expand limited range after decode' (#243).\n"
           "    if (uExpand > 0.5) c = (c - 16.0 / 255.0) * (255.0 / 219.0);\n"
           "    if (uExpand > 1.5) c = (clamp(c, 0.0, 1.0) - 16.0 / 255.0) * (255.0 / 219.0);\n"
           "    c = clamp(c, 0.0, 1.0);\n"
           "    // Saturation around Rec.601 luma (matches the YUV decode).\n"
           "    float y = dot(c, vec3(0.299, 0.587, 0.114));\n"
           "    c = clamp(mix(vec3(y), c, uSaturation), 0.0, 1.0);\n"
           "    c = pow(c, vec3(uInvGamma));\n"
           "    FRAG_OUT = vec4(c, 1.0);\n"
           "}\n";
}

GLuint compileShader(GLenum type, const std::string &src)
{
    GLuint s = glCreateShader(type);
    const char *p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOG_ERROR("SourcePrePass: shader compile failed: " + std::string(log));
        glDeleteShader(s);
        return 0;
    }
    return s;
}
} // namespace

SourcePrePass::~SourcePrePass()
{
    // GL objects are released by destroy() while a context is current;
    // FrameProcessor calls it from its destructor.
}

bool SourcePrePass::isActive(const Params &p)
{
    return p.expandLimitedRange || p.expandAfterDecode || p.deinterlace != 0 ||
           std::fabs(p.saturation - 1.0f) > 1e-4f ||
           std::fabs(p.gamma - 1.0f) > 1e-4f;
}

bool SourcePrePass::ensureProgram()
{
    if (m_program) return true;
    if (m_programFailed) return false;

    GLuint vs = compileShader(GL_VERTEX_SHADER, generateVertexShader());
    GLuint fs = vs ? compileShader(GL_FRAGMENT_SHADER, buildFragmentShader()) : 0;
    if (!vs || !fs)
    {
        if (vs) glDeleteShader(vs);
        m_programFailed = true;
        return false;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    // Attribute slots for the non-"layout" GLSL dialects.
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aTexCoord");
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        LOG_ERROR("SourcePrePass: program link failed: " + std::string(log));
        glDeleteProgram(prog);
        m_programFailed = true;
        return false;
    }
    m_program = prog;
    m_locTex = glGetUniformLocation(prog, "uTex");
    m_locExpand = glGetUniformLocation(prog, "uExpand");
    m_locSaturation = glGetUniformLocation(prog, "uSaturation");
    m_locInvGamma = glGetUniformLocation(prog, "uInvGamma");
    m_locDeint = glGetUniformLocation(prog, "uDeint");
    m_locField = glGetUniformLocation(prog, "uField");
    m_locTexH = glGetUniformLocation(prog, "uTexH");

    // The shared vertex shader carries the output-geometry UV transform
    // (#227). Uniforms default to zero, which would collapse every texcoord
    // to the centre texel and turn this pass into a flat colour: pin it to
    // identity — the source pre-pass never rotates or mirrors.
    {
        const GLint locUv = glGetUniformLocation(prog, "uvXform");
        if (locUv >= 0)
        {
            GLint prevProg = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
            glUseProgram(prog);
            glUniform4f(locUv, 1.0f, 0.0f, 0.0f, 1.0f);
            glUseProgram(static_cast<GLuint>(prevProg));
        }
    }

    // Full-screen quad; texcoords map 1:1 so the output keeps the input's
    // row order (row 0 of the source lands in row 0 of the output).
    const float vertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
    };
    const unsigned int indices[] = {0, 1, 2, 2, 3, 0};

    GLint prevVao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(static_cast<GLuint>(prevVao));

    LOG_INFO("SourcePrePass: program ready");
    return true;
}

bool SourcePrePass::ensureTarget(uint32_t w, uint32_t h)
{
    if (m_fbo && m_outTex && m_outW == w && m_outH == h) return true;

    if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
    if (m_outTex) glDeleteTextures(1, &m_outTex);
    m_fbo = 0;
    m_outTex = 0;
    m_outW = m_outH = 0;

    glGenTextures(1, &m_outTex);
    glBindTexture(GL_TEXTURE_2D, m_outTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    const GLint filter = m_filterLinear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_outTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        LOG_ERROR("SourcePrePass: output framebuffer incomplete");
        glDeleteFramebuffers(1, &m_fbo);
        glDeleteTextures(1, &m_outTex);
        m_fbo = 0;
        m_outTex = 0;
        return false;
    }
    m_outW = w;
    m_outH = h;
    return true;
}

GLuint SourcePrePass::apply(GLuint srcTex, uint32_t w, uint32_t h, const Params &p)
{
    if (srcTex == 0 || w == 0 || h == 0) return 0;

    GLint prevFbo = 0, prevProgram = 0, prevTex = 0, prevVao = 0;
    GLint prevViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_VIEWPORT, prevViewport);

    GLuint result = 0;
    if (ensureProgram() && ensureTarget(w, h))
    {
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glViewport(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
        glDisable(GL_BLEND);
        glUseProgram(m_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, srcTex);
        if (m_locTex >= 0) glUniform1i(m_locTex, 0);
        if (m_locExpand >= 0)
            glUniform1f(m_locExpand, static_cast<float>((p.expandLimitedRange ? 1 : 0) +
                                                        (p.expandAfterDecode ? 1 : 0)));
        if (m_locSaturation >= 0) glUniform1f(m_locSaturation, p.saturation);
        if (m_locInvGamma >= 0)
            glUniform1f(m_locInvGamma, 1.0f / (p.gamma > 0.01f ? p.gamma : 0.01f));
        if (m_locDeint >= 0) glUniform1f(m_locDeint, static_cast<float>(p.deinterlace));
        if (m_locField >= 0) glUniform1f(m_locField, p.field ? 1.0f : 0.0f);
        if (m_locTexH >= 0) glUniform1f(m_locTexH, static_cast<float>(h));
        glBindVertexArray(m_vao);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        result = m_outTex;
    }

    glBindVertexArray(static_cast<GLuint>(prevVao));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    glUseProgram(static_cast<GLuint>(prevProgram));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    return result;
}

void SourcePrePass::setFilterLinear(bool linear)
{
    m_filterLinear = linear;
    if (m_outTex)
    {
        GLint prevTex = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
        glBindTexture(GL_TEXTURE_2D, m_outTex);
        const GLint filter = linear ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    }
}

void SourcePrePass::destroy()
{
    if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
    if (m_outTex) glDeleteTextures(1, &m_outTex);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
    if (m_program) glDeleteProgram(m_program);
    m_fbo = m_outTex = m_vao = m_vbo = m_ebo = m_program = 0;
    m_outW = m_outH = 0;
}
