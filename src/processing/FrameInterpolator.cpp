#include "FrameInterpolator.h"
#include "../utils/Logger.h"

#include <algorithm>
#include <chrono>
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
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_R8
#define GL_R8 0x8229
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_HALF_FLOAT
#define GL_HALF_FLOAT 0x140B
#endif

namespace
{
// Finest flow level is at most this wide (1080p -> 480, 720p -> 640).
constexpr uint32_t kMaxFlowWidth = 640;
// Repeat detection grid (#256) and its "changed block" thresholds (8-bit
// luma steps above the frame's noise floor: block mean / single texel).
constexpr uint32_t kDiffMaxW = 128;
constexpr uint32_t kDiffMaxH = 72;
constexpr int kDiffMeanStep = 3;
constexpr int kDiffMaxStep = 24;
// Block-average channel: 8 units per luma step, so 16 = 2 luma steps.
constexpr int kDiffAvgStep = 16;
// Scene-cut block: average luma moved >= 24 steps (x8 scale; the channel
// saturates at ~31.9 steps).
constexpr int kDiffCutStep = 24 * 8;
constexpr int kMaxLevels = 5;
constexpr uint32_t kMinLevelDim = 16;
// Matching parameters (luma in 0..1, SAD = mean absolute difference).
constexpr int kCoarseRadius = 4;      // full search on the coarsest level
constexpr float kLambda = 0.002f;     // smoothness, per level px of deviation
constexpr float kZeroBias = 0.003f;   // preference for "no motion"

double msSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

GLuint compile(GLenum type, const std::string &src, const char *name)
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
        LOG_ERROR(std::string("FrameInterpolator: ") + name + " compile failed: " + log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

void setI(GLuint p, const char *n, int v)
{
    const GLint l = glGetUniformLocation(p, n);
    if (l >= 0) glUniform1i(l, v);
}
void setF(GLuint p, const char *n, float v)
{
    const GLint l = glGetUniformLocation(p, n);
    if (l >= 0) glUniform1f(l, v);
}
void set2F(GLuint p, const char *n, float a, float b)
{
    const GLint l = glGetUniformLocation(p, n);
    if (l >= 0) glUniform2f(l, a, b);
}

// Saves and restores the GL state the passes touch.
struct GLStateGuard
{
    GLint fbo = 0, prog = 0, vao = 0, active = 0;
    GLint tex[4] = {0, 0, 0, 0};
    GLint vp[4] = {0, 0, 0, 0};
    GLStateGuard()
    {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
        glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetIntegerv(GL_VIEWPORT, vp);
        for (int i = 0; i < 4; ++i)
        {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex[i]);
        }
    }
    ~GLStateGuard()
    {
        for (int i = 3; i >= 0; --i)
        {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(tex[i]));
        }
        glActiveTexture(static_cast<GLenum>(active));
        glBindVertexArray(static_cast<GLuint>(vao));
        glUseProgram(static_cast<GLuint>(prog));
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(fbo));
        glViewport(vp[0], vp[1], vp[2], vp[3]);
    }
};

// ---------------------------------------------------------------- shaders
// Bodies are GLSL 3.30 / ES 3.00 (texture(), in/out); the header picks the
// dialect. Coordinates: p = gl_FragCoord.xy is the pixel centre of the
// target in that level's pixels; textures are sampled at p / size.

const char *kCopyFS = R"(
uniform sampler2D uSrc;
void main() {
    FragColor = vec4(texture(uSrc, TexCoord).rgb, 1.0);
}
)";

// #260 — a reduced RIFE frame scaled back up, with the detail the
// reduction lost taken from the full-size Motion frame: out = up(rife) +
// (motion - up(down(motion))). Both "up"s are the same bilinear sampling of
// same-size textures, so the added part is only the high frequencies the
// reduction removed. Where RIFE and Motion disagree at low frequencies
// (Motion got the motion wrong) the detail is dropped and RIFE stays alone.
const char *kRifeDetailFS = R"(
uniform sampler2D uRife;        // reduced RIFE frame (LINEAR)
uniform sampler2D uMotionSmall; // Motion frame reduced the same way (LINEAR)
uniform sampler2D uMotion;      // full-size Motion frame (NEAREST)
const vec3 W = vec3(0.299, 0.587, 0.114);
void main() {
    vec3 r = texture(uRife, TexCoord).rgb;
    vec3 ms = texture(uMotionSmall, TexCoord).rgb;
    vec3 m = texture(uMotion, TexCoord).rgb;
    float d = abs(dot(r - ms, W));
    float keep = 1.0 - smoothstep(0.03, 0.10, d);
    FragColor = vec4(clamp(r + keep * (m - ms), 0.0, 1.0), 1.0);
}
)";

// Source -> level-0 luma: box average of uScale x uScale source pixels,
// taken as (uScale/2)^2 bilinear taps on texel corners (LINEAR sampler).
const char *kLumaFS = R"(
uniform sampler2D uSrc;
uniform vec2 uSrcSize;
uniform int uScale;
const vec3 W = vec3(0.299, 0.587, 0.114);
void main() {
    vec2 base = floor(gl_FragCoord.xy) * float(uScale);
    float acc = 0.0;
    float n = 0.0;
    if (uScale <= 1) {
        acc = dot(texture(uSrc, (base + 0.5) / uSrcSize).rgb, W);
        n = 1.0;
    } else {
        int k = uScale / 2;
        for (int j = 0; j < k; ++j)
            for (int i = 0; i < k; ++i) {
                vec2 q = base + vec2(float(2 * i + 1), float(2 * j + 1));
                acc += dot(texture(uSrc, q / uSrcSize).rgb, W);
                n += 1.0;
            }
    }
    FragColor = vec4(acc / n, 0.0, 0.0, 1.0);
}
)";

// 2x2 box downsample (one bilinear tap on the corner of the 2x2 block).
const char *kDownFS = R"(
uniform sampler2D uSrc;
uniform vec2 uSrcSize;
void main() {
    vec2 q = floor(gl_FragCoord.xy) * 2.0 + 1.0;
    FragColor = vec4(texture(uSrc, q / uSrcSize).r, 0.0, 0.0, 1.0);
}
)";

// Repeat detection (#256): one texel per block of the source.
//  R = mean |dLuma| over a 4x4 grid of exact texels (pixel-level change);
//  G = max of those;
//  B = |mean luma A - mean luma B| over the whole block (8x8 bilinear taps,
//      ~16x16 texels): blind to sub-pixel shifts (the field shimmer of a
//      bob-deinterlaced 480i source, scaler ringing), not to real motion.
const char *kDiffFS = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform vec2 uBlock;
uniform vec2 uSize;
const vec3 W = vec3(0.299, 0.587, 0.114);
void main() {
    ivec2 sz = textureSize(uA, 0);
    vec2 base = floor(gl_FragCoord.xy) * uBlock;
    float acc = 0.0;
    float mx = 0.0;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            vec2 q = base + (vec2(float(i), float(j)) + 0.5) * uBlock * 0.25;
            ivec2 t = clamp(ivec2(q), ivec2(0), sz - 1);
            float d = abs(dot(texelFetch(uA, t, 0).rgb - texelFetch(uB, t, 0).rgb, W));
            acc += d;
            mx = max(mx, d);
        }
    float ma = 0.0;
    float mb = 0.0;
    for (int j = 0; j < 8; ++j)
        for (int i = 0; i < 8; ++i) {
            vec2 q = (base + (vec2(float(i), float(j)) + 0.5) * uBlock * 0.125) / uSize;
            ma += dot(texture(uA, q).rgb, W);
            mb += dot(texture(uB, q).rgb, W);
        }
    FragColor = vec4(acc / 16.0, mx, abs(ma - mb) / 64.0 * 8.0, 1.0);
}
)";

// Shared by the search and consensus passes: bilateral SAD at mid-time.
const char *kMatchCommon = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform vec2 uSize;
uniform float uLambda;
uniform float uZeroBias;
float sad(vec2 p, vec2 v) {
    vec2 h = 0.5 * v;
    float c = 0.0;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            vec2 q = p + vec2(float(i), float(j));
            c += abs(texture(uA, (q - h) / uSize).r - texture(uB, (q + h) / uSize).r);
        }
    return c * (1.0 / 9.0);
}
vec2 gPred;
vec2 gBest;
float gBestScore;
float gBestSad;
void tryVec(vec2 p, vec2 v) {
    float s = sad(p, v);
    vec2 dv = abs(v - gPred);
    float score = s + uLambda * (dv.x + dv.y);
    if (score < gBestScore) {
        gBestScore = score;
        gBest = v;
        gBestSad = s;
    }
}
void refine(vec2 p, float stp) {
    vec2 c = gBest;
    tryVec(p, c + vec2(stp, 0.0));
    tryVec(p, c - vec2(stp, 0.0));
    tryVec(p, c + vec2(0.0, stp));
    tryVec(p, c - vec2(0.0, stp));
}
)";

// One pyramid level of the motion search. Output: rg = vector (this
// level's px, A->B displacement), b = its SAD, a = zero-vector SAD.
const char *kSearchFS = R"(
uniform sampler2D uCoarse;
uniform float uHasCoarse;
uniform vec2 uCoarseScale;
uniform vec2 uCoarseSize;
uniform int uRadius;
void main() {
    vec2 p = gl_FragCoord.xy;
    vec2 uv = p / uSize;
    gPred = vec2(0.0);
    if (uHasCoarse > 0.5)
        gPred = texture(uCoarse, uv).rg * uCoarseScale;
    // Zero motion is a privileged candidate: no smoothness penalty, a bias.
    float zs = sad(p, vec2(0.0));
    gBest = vec2(0.0);
    gBestSad = zs;
    gBestScore = zs - uZeroBias;
    if (uHasCoarse > 0.5) {
        vec2 ct = 1.0 / uCoarseSize;
        tryVec(p, gPred);
        tryVec(p, texture(uCoarse, uv + vec2(ct.x, 0.0)).rg * uCoarseScale);
        tryVec(p, texture(uCoarse, uv - vec2(ct.x, 0.0)).rg * uCoarseScale);
        tryVec(p, texture(uCoarse, uv + vec2(0.0, ct.y)).rg * uCoarseScale);
        tryVec(p, texture(uCoarse, uv - vec2(0.0, ct.y)).rg * uCoarseScale);
    } else {
        for (int y = -uRadius; y <= uRadius; ++y)
            for (int x = -uRadius; x <= uRadius; ++x)
                if (x != 0 || y != 0)
                    tryVec(p, vec2(float(x), float(y)));
    }
    refine(p, 1.0);
    refine(p, 0.5);
    FragColor = vec4(gBest, gBestSad, zs);
}
)";

// Finest level clean-up: among the 3x3 neighbourhood's vectors keep the one
// that matches best HERE (a vector median by cost; removes outliers).
const char *kConsensusFS = R"(
uniform sampler2D uFlow;
void main() {
    vec2 p = gl_FragCoord.xy;
    vec4 c = texture(uFlow, p / uSize);
    gPred = c.rg;
    gBest = c.rg;
    gBestSad = c.b;
    gBestScore = c.b;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i)
            if (i != 0 || j != 0)
                tryVec(p, texture(uFlow, (p + vec2(float(i), float(j))) / uSize).rg);
    if (c.a - uZeroBias < gBestScore) {
        gBest = vec2(0.0);
        gBestSad = c.a;
    }
    FragColor = vec4(gBest, gBestSad, c.a);
}
)";

// Synthesis at phase uT (full source resolution).
const char *kWarpFS = R"(
uniform sampler2D uA;
uniform sampler2D uB;
uniform sampler2D uFlow;
uniform vec2 uFlowSize;
uniform float uT;
uniform float uMethod;
uniform float uProtect;
uniform float uPick;
const vec3 W = vec3(0.299, 0.587, 0.114);
void main() {
    vec2 uv = TexCoord;
    vec3 a0 = texture(uA, uv).rgb;
    vec3 b0 = texture(uB, uv).rgb;
    vec3 blend = mix(a0, b0, uT);
    if (uMethod < 0.5) {
        FragColor = vec4(blend, 1.0);
        return;
    }
    // The field lives on the mid-time plane; the trajectory through uv at
    // time uT crosses it at uv + (0.5 - uT) * v (two fixed-point steps).
    float k = 0.5 - uT;
    vec4 f = texture(uFlow, uv);
    vec2 v = f.rg / uFlowSize;
    if (abs(k) > 1e-4) {
        f = texture(uFlow, uv + k * v);
        v = f.rg / uFlowSize;
        f = texture(uFlow, uv + k * v);
        v = f.rg / uFlowSize;
    }
    vec3 ca = texture(uA, uv - uT * v).rgb;
    vec3 cb = texture(uB, uv + (1.0 - uT) * v).rgb;
    float d = abs(dot(ca - cb, W));
    if (uPick > 0.5) {
        // Edge-aware pick: the bilinear vector smears across motion
        // boundaries, so also try the 4 nearest flow texels as they are
        // and keep whichever makes A and B agree best at this pixel.
        vec2 fp = uv * uFlowSize - 0.5;
        vec2 f0 = floor(fp);
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                vec4 g = texture(uFlow, (f0 + vec2(float(i), float(j)) + 0.5) / uFlowSize);
                vec2 w = g.rg / uFlowSize;
                vec3 xa = texture(uA, uv - uT * w).rgb;
                vec3 xb = texture(uB, uv + (1.0 - uT) * w).rgb;
                float e = abs(dot(xa - xb, W));
                if (e < d) {
                    d = e;
                    ca = xa;
                    cb = xb;
                    f = g;
                }
            }
    }
    vec3 mc = mix(ca, cb, uT);
    // Confidence: the block SAD of the chosen vector and the per-pixel
    // disagreement of the two warped frames; unreliable -> plain blend.
    float err = max(f.b, d);
    float conf = 1.0 - smoothstep(0.06, 0.20, err);
    vec3 c = mix(blend, mc, conf);
    if (uProtect > 0.5) {
        // Static region (HUD, letterbox): unchanged here and around.
        float d0 = abs(dot(a0 - b0, W));
        if (f.a < 0.006 && d0 < 0.02)
            c = blend;
    }
    FragColor = vec4(c, 1.0);
}
)";
} // namespace

bool FrameInterpolator::isSupported()
{
    return ensurePrograms();
}

GLuint FrameInterpolator::buildProgram(const std::string &fragBody, const char *name)
{
    std::string vsHeader, fsHeader;
    if (m_isES)
    {
        vsHeader = "#version 300 es\nprecision highp float;\nin vec2 aPos;\n";
        fsHeader = "#version 300 es\nprecision highp float;\nprecision highp int;\n"
                   "precision highp sampler2D;\nin vec2 TexCoord;\nout vec4 FragColor;\n";
    }
    else
    {
        vsHeader = "#version 330 core\nlayout(location = 0) in vec2 aPos;\n";
        fsHeader = "#version 330 core\nin vec2 TexCoord;\nout vec4 FragColor;\n";
    }
    // Own vertex shader (not the renderer's): no uvXform to pin (#227).
    const std::string vs = vsHeader +
                           "out vec2 TexCoord;\n"
                           "void main() {\n"
                           "    TexCoord = aPos * 0.5 + 0.5;\n"
                           "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
                           "}\n";
    GLuint v = compile(GL_VERTEX_SHADER, vs, name);
    GLuint f = v ? compile(GL_FRAGMENT_SHADER, fsHeader + fragBody, name) : 0;
    if (!v || !f)
    {
        if (v) glDeleteShader(v);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glBindAttribLocation(prog, 0, "aPos");
    glLinkProgram(prog);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        LOG_ERROR(std::string("FrameInterpolator: ") + name + " link failed: " + log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

bool FrameInterpolator::ensurePrograms()
{
    if (m_programsTried) return m_programsOk;
    m_programsTried = true;

    const int major = getOpenGLMajorVersion();
    m_isES = isOpenGLES();
    if (major < 3)
    {
        m_failReason = "needs OpenGL 3.3 / OpenGL ES 3.0";
        LOG_WARN("FrameInterpolator: unsupported (" + m_failReason + ")");
        return false;
    }

    GLStateGuard guard;
    const std::string match = kMatchCommon;
    m_progCopy = buildProgram(kCopyFS, "copy");
    m_progLuma = buildProgram(kLumaFS, "luma");
    m_progDown = buildProgram(kDownFS, "down");
    m_progSearch = buildProgram(match + kSearchFS, "search");
    m_progConsensus = buildProgram(match + kConsensusFS, "consensus");
    m_progWarp = buildProgram(kWarpFS, "warp");
    m_progDiff = buildProgram(kDiffFS, "diff");
    m_progRifeDetail = buildProgram(kRifeDetailFS, "rife-detail");
    if (!m_progCopy || !m_progLuma || !m_progDown || !m_progSearch || !m_progConsensus ||
        !m_progWarp || !m_progDiff || !m_progRifeDetail)
    {
        m_failReason = "shader compilation failed";
        destroy();
        m_programsTried = true;
        return false;
    }

    // Full-screen quad, positions only.
    const float verts[] = {-1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f};
    const unsigned int idx[] = {0, 1, 2, 2, 3, 0};
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
    glBindVertexArray(0);

    // Motion vectors need a float render target (core in GL 3.x; GLES 3.0
    // only with EXT_color_buffer_(half_)float). Probe it.
    {
        Target probe;
        m_motionOk = makeTarget(probe, 8, 8, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, true);
        freeTarget(probe);
        if (!m_motionOk)
            LOG_WARN("FrameInterpolator: no float render targets; motion mode falls back to blend");
    }

    m_programsOk = true;
    LOG_INFO(std::string("FrameInterpolator: programs ready (") + (m_isES ? "GLSL ES 3.00" : "GLSL 3.30") +
             (m_motionOk ? ", motion + blend)" : ", blend only)"));
    return true;
}

bool FrameInterpolator::makeTarget(Target &t, uint32_t w, uint32_t h, GLint internalFmt,
                                   GLenum fmt, GLenum type, bool linear)
{
    freeTarget(t);
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0,
                 fmt, type, nullptr);
    const GLint filter = linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok)
    {
        freeTarget(t);
        return false;
    }
    t.w = w;
    t.h = h;
    return true;
}

void FrameInterpolator::freeTarget(Target &t)
{
    if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
    if (t.tex) glDeleteTextures(1, &t.tex);
    t = Target();
}

void FrameInterpolator::releaseTargets()
{
    m_rife.reset(); // new geometry: the held frames no longer pair
    for (Slot &s : m_slots)
    {
        freeTarget(s.color);
        for (Target &t : s.luma) freeTarget(t);
        s.luma.clear();
    }
    for (Target &t : m_flow) freeTarget(t);
    m_flow.clear();
    freeTarget(m_flowFinal);
    freeTarget(m_diff);
    freeTarget(m_out);
    freeTarget(m_rifeIn);
    freeTarget(m_rifeRes);
    freeTarget(m_rifeMotion);
    freeTarget(m_rifeMotionSmall);
    m_rifeW = m_rifeH = 0;
    m_levels.clear();
    m_w = m_h = 0;
    m_count = 0;
    m_latest = -1;
    m_flowValid = false;
}

bool FrameInterpolator::ensureTargets(uint32_t w, uint32_t h)
{
    if (m_w == w && m_h == h && m_out.tex) return true;
    releaseTargets();

    // Working size: halve until the finest flow level fits kMaxFlowWidth.
    m_scale = 1;
    while (m_scale < 8 && (w + m_scale - 1) / m_scale > kMaxFlowWidth) m_scale *= 2;
    LevelSize l0{(w + m_scale - 1) / m_scale, (h + m_scale - 1) / m_scale};
    m_levels.push_back(l0);
    while (static_cast<int>(m_levels.size()) < kMaxLevels)
    {
        const LevelSize &p = m_levels.back();
        LevelSize n{(p.w + 1) / 2, (p.h + 1) / 2};
        if (n.w < kMinLevelDim || n.h < kMinLevelDim) break;
        m_levels.push_back(n);
    }

    bool ok = makeTarget(m_out, w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, m_filterLinear);
    for (Slot &s : m_slots)
    {
        ok = ok && makeTarget(s.color, w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, m_filterLinear);
        s.luma.resize(m_levels.size());
        for (size_t i = 0; ok && i < m_levels.size(); ++i)
            ok = makeTarget(s.luma[i], m_levels[i].w, m_levels[i].h, GL_R8, GL_RED, GL_UNSIGNED_BYTE, true);
    }
    {
        // Repeat-detection grid: blocks of >= 8 source px, at most 128x72.
        const uint32_t dw = std::max<uint32_t>(1, std::min<uint32_t>(kDiffMaxW, w / 8));
        const uint32_t dh = std::max<uint32_t>(1, std::min<uint32_t>(kDiffMaxH, h / 8));
        ok = ok && makeTarget(m_diff, dw, dh, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, false);
    }
    if (ok && m_motionOk)
    {
        m_flow.resize(m_levels.size());
        for (size_t i = 0; ok && i < m_levels.size(); ++i)
            ok = makeTarget(m_flow[i], m_levels[i].w, m_levels[i].h, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, true);
        ok = ok && makeTarget(m_flowFinal, m_levels[0].w, m_levels[0].h, GL_RGBA16F, GL_RGBA,
                              GL_HALF_FLOAT, true);
    }
    if (!ok)
    {
        LOG_ERROR("FrameInterpolator: could not allocate render targets for " + std::to_string(w) +
                  "x" + std::to_string(h));
        releaseTargets();
        return false;
    }
    m_w = w;
    m_h = h;
    LOG_INFO("FrameInterpolator: " + std::to_string(w) + "x" + std::to_string(h) + ", flow " +
             std::to_string(m_levels[0].w) + "x" + std::to_string(m_levels[0].h) + " (1/" +
             std::to_string(m_scale) + "), " + std::to_string(m_levels.size()) + " levels");
    return true;
}

void FrameInterpolator::drawTo(const Target &t)
{
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glViewport(0, 0, static_cast<GLsizei>(t.w), static_cast<GLsizei>(t.h));
    glBindVertexArray(m_vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
}

void FrameInterpolator::bindTex(int unit, GLuint tex, bool linear)
{
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
    const GLint f = linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
}

void FrameInterpolator::restoreFilters()
{
    // The colour textures are handed out to the rest of the pipeline:
    // present them with the source's filter, like FrameProcessor does.
    const GLint f = m_filterLinear ? GL_LINEAR : GL_NEAREST;
    GLuint texs[3] = {m_slots[0].color.tex, m_slots[1].color.tex, m_out.tex};
    for (GLuint t : texs)
    {
        if (!t) continue;
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    }
}

void FrameInterpolator::buildPyramid(Slot &s)
{
    glUseProgram(m_progLuma);
    bindTex(0, s.color.tex, true);
    setI(m_progLuma, "uSrc", 0);
    set2F(m_progLuma, "uSrcSize", static_cast<float>(m_w), static_cast<float>(m_h));
    setI(m_progLuma, "uScale", m_scale);
    drawTo(s.luma[0]);

    glUseProgram(m_progDown);
    setI(m_progDown, "uSrc", 0);
    for (size_t i = 1; i < s.luma.size(); ++i)
    {
        bindTex(0, s.luma[i - 1].tex, true);
        set2F(m_progDown, "uSrcSize", static_cast<float>(s.luma[i - 1].w),
              static_cast<float>(s.luma[i - 1].h));
        drawTo(s.luma[i]);
    }
}

bool FrameInterpolator::pushFrame(GLuint srcTex, uint32_t w, uint32_t h)
{
    if (!srcTex || w == 0 || h == 0 || !ensurePrograms()) return false;
    GLStateGuard guard;
    if (!ensureTargets(w, h)) return false;

    const auto t0 = std::chrono::steady_clock::now();
    const int slot = (m_latest < 0) ? 0 : 1 - m_latest;
    Slot &s = m_slots[slot];

    glDisable(GL_BLEND);
    glUseProgram(m_progCopy);
    // Copy at texel centres: exact whatever the source's filter is, so a
    // real frame handed back out is bit-identical to the source.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, srcTex);
    setI(m_progCopy, "uSrc", 0);
    drawTo(s.color);
    if (m_motionOk) buildPyramid(s);
    restoreFilters();
    if (m_rifeWanted)
    {
        // #260 — the network gets a reduced copy above 720p (Auto / Fast).
        uint32_t rw = w, rh = h;
        GLuint rifeFbo = s.color.fbo;
        if (rifeReducedSize(m_rifeScale, w, h, rw, rh))
        {
            bool ok = m_rifeIn.w == rw && m_rifeIn.h == rh && m_rifeRes.w == rw && m_rifeRes.h == rh;
            if (!ok)
                ok = makeTarget(m_rifeIn, rw, rh, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, true) &&
                     makeTarget(m_rifeRes, rw, rh, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, true);
            if (ok)
            {
                glBindFramebuffer(GL_READ_FRAMEBUFFER, s.color.fbo);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_rifeIn.fbo);
                glBlitFramebuffer(0, 0, static_cast<GLint>(w), static_cast<GLint>(h), 0, 0,
                                  static_cast<GLint>(rw), static_cast<GLint>(rh), GL_COLOR_BUFFER_BIT, GL_LINEAR);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                rifeFbo = m_rifeIn.fbo;
            }
            else
            {
                rw = w;
                rh = h;
            }
        }
        m_rifeW = rw;
        m_rifeH = rh;
        m_rife.submitFrame(rifeFbo, rw, rh);
    }

    m_latest = slot;
    if (m_count < 2) ++m_count;
    m_flowValid = false;
    if (m_profile)
    {
        glFinish();
        m_timings.pushMs = msSince(t0);
    }
    return true;
}

FrameInterpolator::Difference FrameInterpolator::compareWithLatest(GLuint srcTex, uint32_t w, uint32_t h)
{
    Difference d;
    const GLuint b = latestTexture();
    if (!srcTex || !b || w != m_w || h != m_h || !m_diff.fbo || !m_programsOk) return d;
    GLStateGuard guard;
    glDisable(GL_BLEND);
    glUseProgram(m_progDiff);
    bindTex(0, srcTex, true);
    bindTex(1, b, true);
    setI(m_progDiff, "uA", 0);
    set2F(m_progDiff, "uSize", static_cast<float>(w), static_cast<float>(h));
    setI(m_progDiff, "uB", 1);
    set2F(m_progDiff, "uBlock", static_cast<float>(w) / static_cast<float>(m_diff.w),
          static_cast<float>(h) / static_cast<float>(m_diff.h));
    drawTo(m_diff);
    // The source texture is shared with the rest of the pipeline: give it
    // back its own filter (B is one of ours).
    bindTex(0, srcTex, m_filterLinear);
    restoreFilters();

    const size_t n = static_cast<size_t>(m_diff.w) * m_diff.h;
    m_diffPixels.resize(n * 4);
    // RGBA8 rows are 4-byte aligned: the default pack alignment fits.
    glReadPixels(0, 0, static_cast<GLsizei>(m_diff.w), static_cast<GLsizei>(m_diff.h), GL_RGBA,
                 GL_UNSIGNED_BYTE, m_diffPixels.data());

    // Noise floor = median block mean; a block "changed" when its mean is
    // clearly above it, or a single texel in it jumped (a small sprite).
    // The block-average channel (x8 scale, 1/8 luma step) counts its own
    // changed blocks: that one ignores sub-pixel shimmer.
    std::vector<uint8_t> means(n), avg(n);
    for (size_t i = 0; i < n; ++i)
    {
        means[i] = m_diffPixels[i * 4];
        avg[i] = m_diffPixels[i * 4 + 2];
    }
    std::vector<uint8_t> sorted = means;
    std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.end());
    const int noise = sorted[n / 2];
    sorted = avg;
    std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.end());
    const int avgNoise = sorted[n / 2];
    int peak = 0, changed = 0, changedAvg = 0, peakAvg = 0, cutBlocks = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const int m = means[i];
        const int mx = m_diffPixels[i * 4 + 1];
        if (m > peak) peak = m;
        if (m > noise + kDiffMeanStep || mx > 2 * noise + kDiffMaxStep) ++changed;
        if (avg[i] > peakAvg) peakAvg = avg[i];
        if (avg[i] > avgNoise + kDiffAvgStep) ++changedAvg;
        if (avg[i] >= kDiffCutStep) ++cutBlocks;
    }
    d.changedAvg = changedAvg;
    d.cutBlocks = cutBlocks;
    d.peakAvg = peakAvg / 8.0f / 255.0f;
    d.valid = true;
    d.noise = noise / 255.0f;
    d.peak = peak / 255.0f;
    d.changedBlocks = changed;
    d.blocks = static_cast<int>(n);
    return d;
}

void FrameInterpolator::computeFlow()
{
    const Slot &A = m_slots[1 - m_latest];
    const Slot &B = m_slots[m_latest];
    const int top = static_cast<int>(m_levels.size()) - 1;

    glUseProgram(m_progSearch);
    setI(m_progSearch, "uA", 0);
    setI(m_progSearch, "uB", 1);
    setI(m_progSearch, "uCoarse", 2);
    setF(m_progSearch, "uLambda", kLambda);
    setF(m_progSearch, "uZeroBias", kZeroBias);
    setI(m_progSearch, "uRadius", kCoarseRadius);
    for (int l = top; l >= 0; --l)
    {
        bindTex(0, A.luma[l].tex, true);
        bindTex(1, B.luma[l].tex, true);
        set2F(m_progSearch, "uSize", static_cast<float>(m_levels[l].w), static_cast<float>(m_levels[l].h));
        if (l < top)
        {
            bindTex(2, m_flow[l + 1].tex, true);
            setF(m_progSearch, "uHasCoarse", 1.0f);
            set2F(m_progSearch, "uCoarseScale",
                  static_cast<float>(m_levels[l].w) / static_cast<float>(m_levels[l + 1].w),
                  static_cast<float>(m_levels[l].h) / static_cast<float>(m_levels[l + 1].h));
            set2F(m_progSearch, "uCoarseSize", static_cast<float>(m_levels[l + 1].w),
                  static_cast<float>(m_levels[l + 1].h));
        }
        else
        {
            bindTex(2, 0, true);
            setF(m_progSearch, "uHasCoarse", 0.0f);
        }
        drawTo(m_flow[l]);
    }

    glUseProgram(m_progConsensus);
    setI(m_progConsensus, "uA", 0);
    setI(m_progConsensus, "uB", 1);
    setI(m_progConsensus, "uFlow", 2);
    setF(m_progConsensus, "uLambda", kLambda);
    setF(m_progConsensus, "uZeroBias", kZeroBias);
    bindTex(0, A.luma[0].tex, true);
    bindTex(1, B.luma[0].tex, true);
    // Neighbour vectors are read at texel centres: NEAREST = exact values.
    bindTex(2, m_flow[0].tex, false);
    set2F(m_progConsensus, "uSize", static_cast<float>(m_levels[0].w), static_cast<float>(m_levels[0].h));
    drawTo(m_flowFinal);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, m_flow[0].tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    m_flowValid = true;
}

void FrameInterpolator::requestPhases(int n)
{
    if (!m_rifeWanted || n < 2 || m_count < 2 || m_rife.state() != RifeBackend::State::Ready) return;
    for (int j = 1; j < n; ++j)
        m_rife.request(static_cast<float>(j) / static_cast<float>(n));
}

bool FrameInterpolator::rifeReducedSize(RifeScale s, uint32_t w, uint32_t h, uint32_t &rw, uint32_t &rh)
{
    rw = w;
    rh = h;
    const uint64_t px = static_cast<uint64_t>(w) * h;
    if (s == RifeScale::Full || w == 0 || h == 0 || px <= kRifeFastPixels + kRifeFastPixels / 8)
        return false; // 720p (and a little above) runs native
    // Same pixel count as 1280x720 whatever the aspect, even sizes.
    const double k = std::sqrt(static_cast<double>(kRifeFastPixels) / static_cast<double>(px));
    rw = static_cast<uint32_t>(std::lround(w * k)) & ~1u;
    rh = static_cast<uint32_t>(std::lround(h * k)) & ~1u;
    if (rw < 64 || rh < 64)
    {
        rw = w;
        rh = h;
        return false;
    }
    return true;
}

void FrameInterpolator::setRifeScale(RifeScale s)
{
    if (s == m_rifeScale) return;
    m_rifeScale = s;
    // The held frames were sent at the old size: start a new pair.
    m_rife.reset();
}

void FrameInterpolator::setMethodHint(Method m)
{
    m_rifeWanted = (m == Method::RIFE);
    if (m_rifeWanted) m_rife.start();
}

GLuint FrameInterpolator::render(float t, const Options &opt)
{
    m_lastRife = false;
    if (m_count < 2 || !m_out.tex) return 0;
    GLStateGuard guard;
    glDisable(GL_BLEND);
    const float tc = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);

    if (opt.method == Method::RIFE)
    {
        setMethodHint(Method::RIFE);
        if (m_rife.state() == RifeBackend::State::Ready)
        {
            const auto t0 = std::chrono::steady_clock::now();
            // #260 — a reduced pair: fetch at its size, scale up to the output.
            const bool reduced = (m_rifeW != m_w || m_rifeH != m_h) && m_rifeRes.tex &&
                                 m_rifeRes.w == m_rifeW && m_rifeRes.h == m_rifeH;
            bool ok = m_rife.fetch(tc, opt.rifeWaitMs, reduced ? m_rifeRes.tex : m_out.tex);
            if (ok && reduced)
                upscaleRife(tc, opt);
            if (m_profile)
            {
                glFinish();
                m_timings.rifeMs = msSince(t0);
            }
            if (ok)
            {
                m_lastRife = true;
                return m_out.tex;
            }
        }
        // Not ready in time (or unavailable): this frame is a Motion one.
    }

    const bool motion = (opt.method == Method::Motion || opt.method == Method::RIFE) && m_motionOk;
    if (motion && !m_flowValid)
    {
        const auto t0 = std::chrono::steady_clock::now();
        computeFlow();
        if (m_profile)
        {
            glFinish();
            m_timings.flowMs = msSince(t0);
        }
    }

    const auto t0 = std::chrono::steady_clock::now();
    drawWarp(tc, opt, motion, m_out);
    if (m_profile)
    {
        glFinish();
        m_timings.warpMs = msSince(t0);
    }
    return m_out.tex;
}

void FrameInterpolator::drawWarp(float tc, const Options &opt, bool motion, const Target &dst)
{
    glUseProgram(m_progWarp);
    bindTex(0, m_slots[1 - m_latest].color.tex, true);
    bindTex(1, m_slots[m_latest].color.tex, true);
    setI(m_progWarp, "uA", 0);
    setI(m_progWarp, "uB", 1);
    setI(m_progWarp, "uFlow", 2);
    if (motion)
    {
        bindTex(2, m_flowFinal.tex, true);
        set2F(m_progWarp, "uFlowSize", static_cast<float>(m_flowFinal.w), static_cast<float>(m_flowFinal.h));
    }
    else
    {
        bindTex(2, 0, true);
    }
    setF(m_progWarp, "uT", tc);
    setF(m_progWarp, "uMethod", motion ? 1.0f : 0.0f);
    setF(m_progWarp, "uProtect", opt.protectStatic ? 1.0f : 0.0f);
    setF(m_progWarp, "uPick", opt.edgeAwarePick ? 1.0f : 0.0f);
    drawTo(dst);
    restoreFilters();
}

void FrameInterpolator::upscaleRife(float tc, const Options &opt)
{
    // #260 — m_rifeRes holds the reduced RIFE frame. Without optical flow it
    // is a plain bilinear scale; with it the Motion frame restores the detail.
    const GLint rw = static_cast<GLint>(m_rifeW), rh = static_cast<GLint>(m_rifeH);
    const GLint w = static_cast<GLint>(m_w), h = static_cast<GLint>(m_h);
    bool detail = m_motionOk;
    if (detail && (m_rifeMotion.w != m_w || m_rifeMotion.h != m_h))
        detail = makeTarget(m_rifeMotion, m_w, m_h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, false);
    if (detail && (m_rifeMotionSmall.w != m_rifeW || m_rifeMotionSmall.h != m_rifeH))
        detail = makeTarget(m_rifeMotionSmall, m_rifeW, m_rifeH, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, true);
    if (!detail)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_rifeRes.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_out.fbo);
        glBlitFramebuffer(0, 0, rw, rh, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    if (!m_flowValid)
        computeFlow();
    drawWarp(tc, opt, true, m_rifeMotion);
    // Reduce the Motion frame exactly as the real frames were for the network.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_rifeMotion.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_rifeMotionSmall.fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, rw, rh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glUseProgram(m_progRifeDetail);
    bindTex(0, m_rifeRes.tex, true);
    bindTex(1, m_rifeMotionSmall.tex, true);
    bindTex(2, m_rifeMotion.tex, false);
    setI(m_progRifeDetail, "uRife", 0);
    setI(m_progRifeDetail, "uMotionSmall", 1);
    setI(m_progRifeDetail, "uMotion", 2);
    drawTo(m_out);
    restoreFilters();
}

GLuint FrameInterpolator::latestTexture() const
{
    return (m_count >= 1 && m_latest >= 0) ? m_slots[m_latest].color.tex : 0;
}

GLuint FrameInterpolator::previousTexture() const
{
    return (m_count >= 2 && m_latest >= 0) ? m_slots[1 - m_latest].color.tex : 0;
}

void FrameInterpolator::reset()
{
    m_rife.reset();
    m_count = 0;
    m_latest = -1;
    m_flowValid = false;
}

void FrameInterpolator::setFilterLinear(bool linear)
{
    if (m_filterLinear == linear) return;
    m_filterLinear = linear;
    if (!m_out.tex) return;
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    restoreFilters();
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev));
}

void FrameInterpolator::destroy()
{
    m_rife.shutdown();
    releaseTargets();
    GLuint *progs[] = {&m_progCopy, &m_progLuma, &m_progDown, &m_progSearch, &m_progConsensus, &m_progWarp,
                       &m_progDiff, &m_progRifeDetail};
    for (GLuint *p : progs)
    {
        if (*p) glDeleteProgram(*p);
        *p = 0;
    }
    if (m_vao) glDeleteVertexArrays(1, &m_vao);
    if (m_vbo) glDeleteBuffers(1, &m_vbo);
    if (m_ebo) glDeleteBuffers(1, &m_ebo);
    m_vao = m_vbo = m_ebo = 0;
    m_programsTried = false;
    m_programsOk = false;
}
