#include "DenoiseBench.h"
#include "TemporalDenoiser.h"
#include "../capture/VideoCaptureTestPattern.h"
#include "../renderer/glad_loader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
using Image = std::vector<uint8_t>; // RGB24

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

// The test source's noise model (VideoCaptureTestPattern, #268): a luma
// part shared by the channels plus chroma speckle, sum-of-uniforms Gaussian.
void addNoise(Image &img, double sigma, uint32_t &s)
{
    if (sigma <= 0.0) return;
    const float k = static_cast<float>(sigma) * 1.732f / 2.0f;
    auto next = [&s]() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(s & 0xFFFF) * (2.0f / 65535.0f) - 1.0f;
    };
    const size_t px = img.size() / 3;
    for (size_t i = 0; i < px; ++i)
    {
        const float luma = (next() + next() + next() + next()) * k * 0.8f;
        for (int c = 0; c < 3; ++c)
        {
            const float chroma = (next() + next()) * k * 0.6f;
            const float v = static_cast<float>(img[i * 3 + c]) + luma + chroma;
            img[i * 3 + c] = static_cast<uint8_t>(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v + 0.5f));
        }
    }
}

double psnr(const Image &a, const Image &b)
{
    double se = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        se += d * d;
    }
    const double mse = se / static_cast<double>(a.size());
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

void writePPM(const std::string &path, const Image &img, uint32_t w, uint32_t h)
{
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    f.write(reinterpret_cast<const char *>(img.data()), static_cast<std::streamsize>(img.size()));
}

double msSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
} // namespace

int runDenoiseBench(const std::string &spec)
{
    auto kv = parseSpec(spec);
    uint32_t w = 640, h = 480;
    // Width a multiple of 4: RGB24 rows then meet GL's default 4-byte
    // unpack alignment (the loader has no glPixelStorei).
    if (kv.count("size") &&
        (std::sscanf(kv["size"].c_str(), "%ux%u", &w, &h) != 2 || w == 0 || h == 0 || (w % 4) != 0))
    {
        std::fprintf(stderr, "denoise-bench: bad size (WxH, W a multiple of 4)\n");
        return 2;
    }
    const int frames = kv.count("frames") ? std::max(20, std::atoi(kv["frames"].c_str())) : 120;
    const double noise = kv.count("noise") ? std::atof(kv["noise"].c_str()) : 8.0;
    std::vector<float> strengths = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    if (kv.count("strengths"))
    {
        strengths.clear();
        std::stringstream ss(kv["strengths"]);
        std::string v;
        while (std::getline(ss, v, ':'))
            strengths.push_back(static_cast<float>(std::atof(v.c_str())));
    }
    const std::string dump = kv.count("dump") ? kv["dump"] : "";

    // Clean clip: moving for the first half, then still on the last moving
    // picture. The noise is regenerated identically for every strength.
    const int moving = frames / 2;
    std::vector<Image> clean(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i)
    {
        clean[static_cast<size_t>(i)].resize(static_cast<size_t>(w) * h * 3);
        const double t = static_cast<double>(std::min(i, moving - 1)) / 60.0;
        VideoCaptureTestPattern::renderMotion(clean[static_cast<size_t>(i)].data(), w, h, t, false);
    }

    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGB,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);

    std::printf("DENOISE bench: %ux%u, %d frames (%d moving, %d still), noise sigma %.1f\n", w, h, frames, moving,
                frames - moving, noise);
    TemporalDenoiser dn;
    // Tuning overrides: t1base, t1slope, t0ratio, pixlo, pixhi, kbase, kslope.
    {
        TemporalDenoiser::Tuning tu;
        auto f = [&](const char *k, float &v) {
            if (kv.count(k)) v = static_cast<float>(std::atof(kv[k].c_str()));
        };
        f("t1base", tu.t1Base);
        f("t1slope", tu.t1Slope);
        f("t0ratio", tu.t0Ratio);
        f("pixlo", tu.pixLo);
        f("pixhi", tu.pixHi);
        f("kbase", tu.kBase);
        f("kslope", tu.kSlope);
        f("line", tu.line);
        dn.setTuning(tu);
    }
    Image out(static_cast<size_t>(w) * h * 3);
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (float s : strengths)
    {
        dn.reset();
        uint32_t seed = 0x9E3779B9u;
        double sumMoving = 0.0, sumStill = 0.0;
        int nMoving = 0, nStill = 0;
        std::vector<double> ms;
        for (int i = 0; i < frames; ++i)
        {
            Image noisy = clean[static_cast<size_t>(i)];
            addNoise(noisy, noise, seed);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), GL_RGB,
                            GL_UNSIGNED_BYTE, noisy.data());
            const Image *result = &noisy;
            if (s > 0.0f)
            {
                glFinish();
                const auto t0 = std::chrono::steady_clock::now();
                const GLuint o = dn.apply(tex, w, h, s);
                glFinish();
                ms.push_back(msSince(t0));
                if (!o)
                {
                    std::fprintf(stderr, "denoise-bench: denoiser failed\n");
                    return 3;
                }
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, o, 0);
                glReadPixels(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), GL_RGBA, GL_UNSIGNED_BYTE,
                             rgba.data());
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                for (size_t p = 0; p < static_cast<size_t>(w) * h; ++p)
                    for (int c = 0; c < 3; ++c)
                        out[p * 3 + c] = rgba[p * 4 + c];
                result = &out;
            }
            const double q = psnr(*result, clean[static_cast<size_t>(i)]);
            // Skip the first frames of each half: the history is settling.
            if (i >= 5 && i < moving)
            {
                sumMoving += q;
                ++nMoving;
            }
            else if (i >= moving + 20)
            {
                sumStill += q;
                ++nStill;
            }
            if (!dump.empty() && i == 20)
            {
                char name[64];
                std::snprintf(name, sizeof(name), "/frame20-s%.2f.ppm", static_cast<double>(s));
                writePPM(dump + name, *result, w, h);
                if (s == strengths.front())
                    writePPM(dump + "/frame20-clean.ppm", clean[20], w, h);
            }
        }
        std::sort(ms.begin(), ms.end());
        std::printf("DENOISE strength=%.2f moving=%.2f still=%.2f ms=%.3f\n", static_cast<double>(s),
                    nMoving ? sumMoving / nMoving : 0.0, nStill ? sumStill / nStill : 0.0,
                    ms.empty() ? 0.0 : ms[ms.size() / 2]);
        std::fflush(stdout);
    }
    dn.destroy();
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    return 0;
}
