#include "FrameGenBench.h"
#include "FrameInterpolator.h"
#include "../capture/VideoCaptureTestPattern.h"
#include "../renderer/glad_loader.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
std::map<std::string, std::string> parseSpec(const std::string &spec)
{
    std::map<std::string, std::string> kv;
    std::stringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ','))
    {
        const size_t eq = item.find('=');
        if (eq != std::string::npos)
            kv[item.substr(0, eq)] = item.substr(eq + 1);
    }
    return kv;
}

// Stage timings. The first pair pays the shader JIT / first-use cost, so
// the median is the number to read; min / max are printed for context.
struct Stat
{
    std::vector<double> v;
    void add(double x) { v.push_back(x); }
    double median() const
    {
        if (v.empty()) return 0.0;
        std::vector<double> s = v;
        std::sort(s.begin(), s.end());
        return s[s.size() / 2];
    }
    double min() const { return v.empty() ? 0.0 : *std::min_element(v.begin(), v.end()); }
    double max() const { return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()); }
};

GLuint makeTexture(uint32_t w, uint32_t h)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

void upload(GLuint tex, const std::vector<uint8_t> &rgb, uint32_t w, uint32_t h, std::vector<uint8_t> &rgba)
{
    rgba.resize(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i)
    {
        rgba[i * 4 + 0] = rgb[i * 3 + 0];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), GL_RGBA,
                    GL_UNSIGNED_BYTE, rgba.data());
}

void readBack(GLuint tex, uint32_t w, uint32_t h, std::vector<uint8_t> &rgba, std::vector<uint8_t> &rgb)
{
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    rgba.resize(static_cast<size_t>(w) * h * 4);
    glReadPixels(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    rgb.resize(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i)
    {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
}
} // namespace

int runFrameGenBench(const std::string &spec)
{
    auto kv = parseSpec(spec);
    const std::string in = kv.count("in") ? kv["in"] : "motion";
    const std::string outDir = kv.count("out") ? kv["out"] : ".";
    const int frames = kv.count("frames") ? std::atoi(kv["frames"].c_str()) : 61;
    const int step = kv.count("step") ? std::atoi(kv["step"].c_str()) : 2;
    const double fps = kv.count("fps") ? std::atof(kv["fps"].c_str()) : 60.0;
    uint32_t w = 1280, h = 720;
    if (kv.count("size"))
        std::sscanf(kv["size"].c_str(), "%ux%u", &w, &h);
    if (w == 0 || h == 0 || frames < step + 1 || step < 2)
    {
        std::fprintf(stderr, "framegen-bench: bad spec '%s'\n", spec.c_str());
        return 2;
    }
    const size_t frameBytes = static_cast<size_t>(w) * h * 3;

    std::ifstream src;
    if (in != "motion")
    {
        src.open(in, std::ios::binary);
        if (!src)
        {
            std::fprintf(stderr, "framegen-bench: cannot open %s\n", in.c_str());
            return 2;
        }
    }
    auto nextFrame = [&](int index, std::vector<uint8_t> &buf) -> bool {
        buf.resize(frameBytes);
        if (in == "motion")
        {
            const double t = 1000.0 + index / fps; // away from t=0 (sprite phases)
            VideoCaptureTestPattern::renderMotion(buf.data(), w, h, t, false);
            return true;
        }
        return static_cast<bool>(src.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(frameBytes)));
    };

    FrameInterpolator fi;
    if (!fi.isSupported())
    {
        std::fprintf(stderr, "framegen-bench: interpolator unsupported: %s\n", fi.unsupportedReason().c_str());
        return 3;
    }
    fi.setProfiling(true);

    // #248 -- RIFE (neural, Vulkan): started before the first frame so every
    // frame reaches it. rife=0 skips it; unavailable = no rife.rgb output.
    bool rife = !(kv.count("rife") && kv["rife"] == "0");
    if (rife)
    {
        fi.setMethodHint(FrameInterpolator::Method::RIFE);
        if (!fi.rife().waitReady(300000))
        {
            std::printf("BENCH rife=unavailable reason=%s\n", fi.rife().reason().c_str());
            fi.setMethodHint(FrameInterpolator::Method::Motion);
            rife = false;
        }
        else
        {
            std::printf("BENCH rife=ready device=%s\n", fi.rife().device().c_str());
        }
    }
    // #260 -- rifescale=auto|full|fast (default auto, as the app).
    if (kv.count("rifescale"))
    {
        const std::string &v = kv["rifescale"];
        fi.setRifeScale(v == "full"   ? FrameInterpolator::RifeScale::Full
                        : v == "fast" ? FrameInterpolator::RifeScale::Fast
                                      : FrameInterpolator::RifeScale::Auto);
    }

    const char *names[] = {"ref", "repeat", "blend", "motion", "motion_nopick", "rife"};
    const int nOuts = rife ? 6 : 5;
    std::ofstream outs[6];
    for (int i = 0; i < nOuts; ++i)
    {
        outs[i].open(outDir + "/" + names[i] + ".rgb", std::ios::binary | std::ios::trunc);
        if (!outs[i])
        {
            std::fprintf(stderr, "framegen-bench: cannot write to %s\n", outDir.c_str());
            return 2;
        }
    }

    GLuint texIn = makeTexture(w, h);
    std::vector<std::vector<uint8_t>> group(static_cast<size_t>(step) + 1);
    std::vector<uint8_t> rgba, rgb;
    Stat push, flow, warpMotion, warpBlend, rifeFetch;
    int generated = 0, rifeFallbacks = 0;

    // Frames 0, K, 2K, ... are kept; the K-1 in between are regenerated.
    int index = 0;
    if (!nextFrame(index++, group[0]))
        return 2;
    upload(texIn, group[0], w, h, rgba);
    fi.pushFrame(texIn, w, h);
    while (index + step - 1 < frames)
    {
        bool ok = true;
        for (int j = 1; j <= step && ok; ++j)
            ok = nextFrame(index++, group[static_cast<size_t>(j)]);
        if (!ok) break;
        upload(texIn, group[static_cast<size_t>(step)], w, h, rgba);
        fi.pushFrame(texIn, w, h);
        push.add(fi.timings().pushMs);

        for (int j = 1; j < step; ++j)
        {
            const float t = static_cast<float>(j) / static_cast<float>(step);
            outs[0].write(reinterpret_cast<const char *>(group[static_cast<size_t>(j)].data()), static_cast<std::streamsize>(frameBytes));
            // Nearest = repeat the closer real frame (what a plain frame-rate
            // conversion shows); at the midpoint that is A.
            const std::vector<uint8_t> &near = (t <= 0.5f) ? group[0] : group[static_cast<size_t>(step)];
            outs[1].write(reinterpret_cast<const char *>(near.data()), static_cast<std::streamsize>(frameBytes));

            FrameInterpolator::Options o;
            o.method = FrameInterpolator::Method::Blend;
            readBack(fi.render(t, o), w, h, rgba, rgb);
            warpBlend.add(fi.timings().warpMs);
            outs[2].write(reinterpret_cast<const char *>(rgb.data()), static_cast<std::streamsize>(frameBytes));

            o.method = FrameInterpolator::Method::Motion;
            GLuint m = fi.render(t, o);
            if (j == 1) flow.add(fi.timings().flowMs);
            warpMotion.add(fi.timings().warpMs);
            readBack(m, w, h, rgba, rgb);
            outs[3].write(reinterpret_cast<const char *>(rgb.data()), static_cast<std::streamsize>(frameBytes));

            o.edgeAwarePick = false;
            readBack(fi.render(t, o), w, h, rgba, rgb);
            outs[4].write(reinterpret_cast<const char *>(rgb.data()), static_cast<std::streamsize>(frameBytes));

            if (rife)
            {
                FrameInterpolator::Options r;
                r.method = FrameInterpolator::Method::RIFE;
                r.rifeWaitMs = -1; // quality run: always wait for the network
                GLuint rt = fi.render(t, r);
                if (fi.lastWasRife())
                    rifeFetch.add(fi.timings().rifeMs);
                else
                    ++rifeFallbacks;
                readBack(rt, w, h, rgba, rgb);
                outs[5].write(reinterpret_cast<const char *>(rgb.data()), static_cast<std::streamsize>(frameBytes));
            }
            ++generated;
        }
        group[0].swap(group[static_cast<size_t>(step)]);
    }
    glDeleteTextures(1, &texIn);

    const char *renderer = reinterpret_cast<const char *>(glGetString(0x1F01 /* GL_RENDERER */));
    std::printf("BENCH renderer=%s size=%ux%u flow=%ux%u levels=%d step=%d generated=%d\n",
                renderer ? renderer : "?", w, h, fi.flowWidth(), fi.flowHeight(), fi.pyramidLevels(), step,
                generated);
    auto line = [](const char *name, const Stat &st, const char *what) {
        std::printf("BENCH %s median=%.3f min=%.3f max=%.3f n=%zu (%s)\n", name, st.median(), st.min(), st.max(),
                    st.v.size(), what);
    };
    line("push_ms", push, "copy + luma pyramid, per real frame");
    line("flow_ms", flow, "motion search, per pair");
    line("warp_motion_ms", warpMotion, "motion synthesis, per generated frame");
    line("warp_blend_ms", warpBlend, "blend synthesis, per generated frame");
    std::printf("BENCH generated_frame_motion_ms=%.3f (push + flow + warp: GPU time per generated frame at 2x)\n",
                push.median() + flow.median() + warpMotion.median());
    if (rife)
    {
        const RifeBackend::Stats rs = fi.rife().stats();
        line("rife_fetch_ms", rifeFetch, "render(): wait for the worker + texture upload, per generated frame");
        std::printf("BENCH rife_infer_ms=%.3f (Vulkan inference + transfers, per generated frame, EMA)\n", rs.inferMs);
        std::printf("BENCH rife_frame_upload_ms=%.3f (host -> Vulkan + pre-process, per real frame, EMA)\n",
                    rs.frameUploadMs);
        std::printf("BENCH rife_gl_readback_ms=%.3f rife_gl_upload_ms=%.3f (GL side of the host round trip, EMA)\n",
                    rs.readbackMs, rs.uploadMs);
        std::printf("BENCH rife_fallbacks=%d (frames shown with Motion instead)\n", rifeFallbacks);
        std::printf("BENCH rife_size=%ux%u (network input; the output is %ux%u)\n", fi.rifeWidth(), fi.rifeHeight(),
                    w, h);
    }
    std::fflush(stdout);
    fi.destroy();
    return generated > 0 ? 0 : 4;
}
