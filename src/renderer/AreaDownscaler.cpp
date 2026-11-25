#include "AreaDownscaler.h"
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

namespace
{
const char *kFS = R"(
uniform sampler2D uSrc;
uniform vec2 uRatio; // source texels per destination pixel (>= 1)
void main() {
    ivec2 sz = textureSize(uSrc, 0);
    vec2 p0 = floor(gl_FragCoord.xy) * uRatio;
    vec2 p1 = p0 + uRatio;
    ivec2 i0 = ivec2(floor(p0));
    ivec2 i1 = min(ivec2(ceil(p1)), sz);
    // Ratios above 16 per axis: stride through the footprint (still an
    // average, just sparser) so the loop stays bounded.
    ivec2 n = i1 - i0;
    ivec2 stride = max(ivec2(1), (n + 15) / 16);
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int y = i0.y; y < i1.y; y += stride.y) {
        float wy = min(float(y + 1), p1.y) - max(float(y), p0.y);
        for (int x = i0.x; x < i1.x; x += stride.x) {
            float w = wy * (min(float(x + 1), p1.x) - max(float(x), p0.x));
            acc += w * texelFetch(uSrc, ivec2(x, y), 0).rgb;
            wsum += w;
        }
    }
    FragColor = vec4(acc / max(wsum, 1e-6), 1.0);
}
)";

GLuint compile(GLenum type, const std::string &src)
{
    GLuint s = glCreateShader(type);
    const char *p = src.c_str();
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOG_ERROR(std::string("AreaDownscaler: shader compile failed: ") + log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}
} // namespace

bool AreaDownscaler::ensure()
{
    if (m_tried) return m_ok;
    m_tried = true;
    if (getOpenGLMajorVersion() < 3)
    {
        LOG_WARN("AreaDownscaler: needs OpenGL 3.3 / GLES 3.0; NEAREST downscale is used");
        return false;
    }
    const bool es = isOpenGLES();
    const std::string vsHead = es ? "#version 300 es\nprecision highp float;\nin vec2 aPos;\n"
                                  : "#version 330 core\nlayout(location = 0) in vec2 aPos;\n";
    const std::string fsHead = es ? "#version 300 es\nprecision highp float;\nprecision highp int;\n"
                                    "precision highp sampler2D;\nout vec4 FragColor;\n"
                                  : "#version 330 core\nout vec4 FragColor;\n";
    const GLuint v = compile(GL_VERTEX_SHADER, vsHead + "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n");
    const GLuint f = v ? compile(GL_FRAGMENT_SHADER, fsHead + kFS) : 0;
    if (!v || !f)
    {
        if (v) glDeleteShader(v);
        return false;
    }
    m_prog = glCreateProgram();
    glAttachShader(m_prog, v);
    glAttachShader(m_prog, f);
    glBindAttribLocation(m_prog, 0, "aPos");
    glLinkProgram(m_prog);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(m_prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        LOG_ERROR("AreaDownscaler: link failed; NEAREST downscale is used");
        glDeleteProgram(m_prog);
        m_prog = 0;
        return false;
    }

    // Full-screen quad (positions only), indexed.
    const float verts[] = {-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f};
    const unsigned int idx[] = {0, 1, 2, 2, 3, 0};
    GLint prevVao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(static_cast<GLuint>(prevVao));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_ok = true;
    LOG_INFO("AreaDownscaler: ready");
    return true;
}

bool AreaDownscaler::render(GLuint srcTex, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
    if (!srcTex || sw == 0 || sh == 0 || dw == 0 || dh == 0 || !ensure()) return false;
    GLint prevProg = 0, prevVao = 0, prevActive = 0, prevTex = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);

    glUseProgram(m_prog);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    glUniform1i(glGetUniformLocation(m_prog, "uSrc"), 0);
    glUniform2f(glGetUniformLocation(m_prog, "uRatio"), static_cast<float>(sw) / static_cast<float>(dw),
                static_cast<float>(sh) / static_cast<float>(dh));
    glDisable(GL_BLEND);
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);

    glBindVertexArray(static_cast<GLuint>(prevVao));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    glActiveTexture(static_cast<GLenum>(prevActive));
    glUseProgram(static_cast<GLuint>(prevProg));
    return true;
}

void AreaDownscaler::destroy()
{
    if (m_prog) glDeleteProgram(m_prog);
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
    m_prog = m_vao = m_vbo = m_ebo = 0;
    m_tried = m_ok = false;
}
