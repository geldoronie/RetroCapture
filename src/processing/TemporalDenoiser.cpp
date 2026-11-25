#include "TemporalDenoiser.h"
#include "../renderer/OpenGLRenderer.h"
#include "../utils/Logger.h"
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
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT 0x140B
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

// Same dialect selection as SourcePrePass; the body uses SRC_TEX / FRAG_OUT.
std::string buildFragmentShader()
{
    const std::string version = trimmedGLSLVersion();
    const bool isES = isOpenGLES();
    const int major = getOpenGLMajorVersion();

    std::string header;
    if (isES && major >= 3)
        header = version + "\nprecision highp float;\nin vec2 TexCoord;\nout vec4 FragColor;\n"
                           "#define SRC_TEX texture\n#define FRAG_OUT FragColor\n";
    else if (isES)
        header = version + "\nprecision mediump float;\nvarying vec2 TexCoord;\n"
                           "#define SRC_TEX texture2D\n#define FRAG_OUT gl_FragColor\n";
    else if (major >= 3)
        header = version + " core\nin vec2 TexCoord;\nout vec4 FragColor;\n"
                           "#define SRC_TEX texture\n#define FRAG_OUT FragColor\n";
    else
        header = version + "\nvarying vec2 TexCoord;\n#define SRC_TEX texture2D\n#define FRAG_OUT gl_FragColor\n";

    return header +
           "uniform sampler2D uCur;\n"
           "uniform sampler2D uPrev;\n"
           "uniform vec2 uTexel;\n"   // 1 / size
           "uniform float uT0;\n"     // box-luma difference: full averaging below
           "uniform float uT1;\n"     // ... none above
           "uniform float uKMax;\n"   // recursion weight on still areas
           "uniform float uFirst;\n"  // no history yet: pass through
           "uniform float uPixLo;\n"  // per-pixel change gate, x uT1
           "uniform float uPixHi;\n"
           "uniform float uLine;\n"   // weight of the row / column sums
           "const vec3 W = vec3(0.299, 0.587, 0.114);\n"
           "void main() {\n"
           "    vec3 c = SRC_TEX(uCur, TexCoord).rgb;\n"
           "    if (uFirst > 0.5) { FRAG_OUT = vec4(c, 1.0); return; }\n"
           "    vec3 p = SRC_TEX(uPrev, TexCoord).rgb;\n"
           "    // Motion detector on the signed luma difference: summed over the\n"
           "    // 3x3 box zero-mean noise cancels (~1/3) while a real change adds\n"
           "    // up. A thin edge moving by a pixel leaves +d on one side and -d\n"
           "    // on the other, which cancel in the box: the per-row and\n"
           "    // per-column sums keep them apart (noise ~1/1.7 there).\n"
           "    float r0 = 0.0, r1 = 0.0, r2 = 0.0, c0 = 0.0, c1 = 0.0, c2 = 0.0;\n"
           "    for (int dy = -1; dy <= 1; ++dy) {\n"
           "        for (int dx = -1; dx <= 1; ++dx) {\n"
           "            vec2 o = TexCoord + vec2(float(dx), float(dy)) * uTexel;\n"
           "            float d = dot(SRC_TEX(uCur, o).rgb - SRC_TEX(uPrev, o).rgb, W);\n"
           "            if (dy < 0) r0 += d; else if (dy > 0) r2 += d; else r1 += d;\n"
           "            if (dx < 0) c0 += d; else if (dx > 0) c2 += d; else c1 += d;\n"
           "        }\n"
           "    }\n"
           "    float line = max(max(max(abs(r0), abs(r1)), abs(r2)), max(max(abs(c0), abs(c1)), abs(c2)));\n"
           "    float dm = max(abs(r0 + r1 + r2) / 9.0, uLine * line / 3.0);\n"
           "    // A large change at the pixel itself (thin edge, small sprite)\n"
           "    // also stops the averaging, on luma or on any channel.\n"
           "    vec3 dd = abs(c - p);\n"
           "    float dp = max(abs(dot(c - p, W)), 0.7 * max(max(dd.r, dd.g), dd.b));\n"
           "    float k = uKMax * (1.0 - smoothstep(uT0, uT1, dm))\n"
           "                    * (1.0 - smoothstep(uPixLo * uT1, uPixHi * uT1, dp));\n"
           "    FRAG_OUT = vec4(mix(c, p, k), 1.0);\n"
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
        LOG_ERROR("TemporalDenoiser: shader compile failed: " + std::string(log));
        glDeleteShader(s);
        return 0;
    }
    return s;
}
} // namespace

bool TemporalDenoiser::ensureProgram()
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
        LOG_ERROR("TemporalDenoiser: program link failed: " + std::string(log));
        glDeleteProgram(prog);
        m_programFailed = true;
        return false;
    }
    m_program = prog;
    m_locCur = glGetUniformLocation(prog, "uCur");
    m_locPrev = glGetUniformLocation(prog, "uPrev");
    m_locTexel = glGetUniformLocation(prog, "uTexel");
    m_locT0 = glGetUniformLocation(prog, "uT0");
    m_locT1 = glGetUniformLocation(prog, "uT1");
    m_locKMax = glGetUniformLocation(prog, "uKMax");
    m_locFirst = glGetUniformLocation(prog, "uFirst");
    m_locPixLo = glGetUniformLocation(prog, "uPixLo");
    m_locPixHi = glGetUniformLocation(prog, "uPixHi");
    m_locLine = glGetUniformLocation(prog, "uLine");

    // The shared vertex shader carries the output-geometry UV transform
    // (#227): pin it to identity, as SourcePrePass does.
    const GLint locUv = glGetUniformLocation(prog, "uvXform");
    if (locUv >= 0)
    {
        GLint prevProg = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
        glUseProgram(prog);
        glUniform4f(locUv, 1.0f, 0.0f, 0.0f, 1.0f);
        glUseProgram(static_cast<GLuint>(prevProg));
    }

    // Full-screen quad, texcoords 1:1 (output keeps the input's row order).
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
    LOG_INFO("TemporalDenoiser: program ready");
    return true;
}

bool TemporalDenoiser::makeTarget(int i, uint32_t w, uint32_t h, bool half)
{
    if (m_fbo[i]) glDeleteFramebuffers(1, &m_fbo[i]);
    if (m_tex[i]) glDeleteTextures(1, &m_tex[i]);
    m_fbo[i] = m_tex[i] = 0;

    glGenTextures(1, &m_tex[i]);
    glBindTexture(GL_TEXTURE_2D, m_tex[i]);
    if (half)
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGBA,
                     GL_HALF_FLOAT, nullptr);
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
    const GLint filter = m_filterLinear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &m_fbo[i]);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_tex[i], 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!ok)
    {
        glDeleteFramebuffers(1, &m_fbo[i]);
        glDeleteTextures(1, &m_tex[i]);
        m_fbo[i] = m_tex[i] = 0;
    }
    return ok;
}

bool TemporalDenoiser::ensureTargets(uint32_t w, uint32_t h)
{
    if (m_fbo[0] && m_fbo[1] && m_w == w && m_h == h) return true;
    m_valid = false;
    m_w = m_h = 0;
    bool ok = false;
    if (!m_halfFailed)
    {
        ok = makeTarget(0, w, h, true) && makeTarget(1, w, h, true);
        if (!ok)
        {
            m_halfFailed = true;
            LOG_INFO("TemporalDenoiser: RGBA16F not renderable, using RGBA8 history");
        }
    }
    if (!ok)
        ok = makeTarget(0, w, h, false) && makeTarget(1, w, h, false);
    if (!ok)
    {
        LOG_ERROR("TemporalDenoiser: output framebuffer incomplete");
        return false;
    }
    m_w = w;
    m_h = h;
    return true;
}

GLuint TemporalDenoiser::apply(GLuint srcTex, uint32_t w, uint32_t h, float strength)
{
    if (srcTex == 0 || w == 0 || h == 0) return 0;

    GLint prevFbo = 0, prevProgram = 0, prevVao = 0, prevActive = 0;
    GLint prevTex[2] = {0, 0};
    GLint prevViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    for (int i = 0; i < 2; ++i)
    {
        glActiveTexture(GL_TEXTURE0 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex[i]);
    }

    GLuint result = 0;
    if (ensureProgram() && ensureTargets(w, h))
    {
        const float s = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);
        // Strength scales how large a box-luma change still counts as noise
        // (~4/255 .. 19/255) and how much history still areas keep
        // (0.5 .. 0.9: ~3 .. ~19 frames of averaging).
        const Tuning &tu = m_tuning;
        const float t1 = (tu.t1Base + tu.t1Slope * s) / 255.0f;
        const float t0 = tu.t0Ratio * t1;
        const float kMax = tu.kBase + tu.kSlope * s;

        const int dst = m_valid ? 1 - m_last : 0;
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[dst]);
        glViewport(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
        glDisable(GL_BLEND);
        glUseProgram(m_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, srcTex);
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, m_valid ? m_tex[m_last] : srcTex);
        if (m_locCur >= 0) glUniform1i(m_locCur, 0);
        if (m_locPrev >= 0) glUniform1i(m_locPrev, 1);
        if (m_locTexel >= 0) glUniform2f(m_locTexel, 1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h));
        if (m_locT0 >= 0) glUniform1f(m_locT0, t0);
        if (m_locT1 >= 0) glUniform1f(m_locT1, t1);
        if (m_locKMax >= 0) glUniform1f(m_locKMax, kMax);
        if (m_locFirst >= 0) glUniform1f(m_locFirst, m_valid ? 0.0f : 1.0f);
        if (m_locPixLo >= 0) glUniform1f(m_locPixLo, tu.pixLo);
        if (m_locPixHi >= 0) glUniform1f(m_locPixHi, tu.pixHi);
        if (m_locLine >= 0) glUniform1f(m_locLine, tu.line);
        glBindVertexArray(m_vao);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
        m_last = dst;
        m_valid = true;
        result = m_tex[dst];
    }

    glBindVertexArray(static_cast<GLuint>(prevVao));
    for (int i = 1; i >= 0; --i)
    {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex[i]));
    }
    glActiveTexture(static_cast<GLenum>(prevActive));
    glUseProgram(static_cast<GLuint>(prevProgram));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    return result;
}

void TemporalDenoiser::setFilterLinear(bool linear)
{
    if (m_filterLinear == linear) return;
    m_filterLinear = linear;
    GLint prevTex = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    const GLint filter = linear ? GL_LINEAR : GL_NEAREST;
    for (GLuint t : m_tex)
    {
        if (!t) continue;
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    }
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
}

void TemporalDenoiser::destroy()
{
    for (int i = 0; i < 2; ++i)
    {
        if (m_fbo[i]) glDeleteFramebuffers(1, &m_fbo[i]);
        if (m_tex[i]) glDeleteTextures(1, &m_tex[i]);
        m_fbo[i] = m_tex[i] = 0;
    }
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
    if (m_program) glDeleteProgram(m_program);
    m_vao = m_vbo = m_ebo = m_program = 0;
    m_w = m_h = 0;
    m_valid = false;
}
