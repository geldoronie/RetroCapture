#include "UpscalerBench.h"
#include "UpscalerSettings.h"
#include "UpscalerStage.h"
#include "../capture/VideoCaptureTestPattern.h"
#include "../renderer/glad_loader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace
{
using Image = std::vector<uint8_t>; // RGB24

// std::clamp is C++17; the old MinGW of the Windows build lacks it.
double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

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

bool parseSize(std::map<std::string, std::string> &kv, const char *key, uint32_t &w, uint32_t &h)
{
    if (!kv.count(key)) return true;
    return std::sscanf(kv[key].c_str(), "%ux%u", &w, &h) == 2 && w > 0 && h > 0;
}

Image renderEdges(uint32_t w, uint32_t h)
{
    Image img(static_cast<size_t>(w) * h * 3);
    VideoCaptureTestPattern::renderEdgesTo(img.data(), w, h);
    return img;
}

// ---------------------------------------------------------------- inputs
Image decimateNearest(const Image &src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
    // Same sampling as a GL_NEAREST render of the source to a dw x dh target.
    Image out(static_cast<size_t>(dw) * dh * 3);
    for (uint32_t y = 0; y < dh; ++y)
    {
        const uint32_t sy = std::min(sh - 1, static_cast<uint32_t>((y + 0.5) * sh / dh));
        for (uint32_t x = 0; x < dw; ++x)
        {
            const uint32_t sx = std::min(sw - 1, static_cast<uint32_t>((x + 0.5) * sw / dw));
            for (int c = 0; c < 3; ++c)
                out[(static_cast<size_t>(y) * dw + x) * 3 + c] = src[(static_cast<size_t>(sy) * sw + sx) * 3 + c];
        }
    }
    return out;
}

Image decimateArea(const Image &src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
    // Exact box filter: each destination pixel averages the source area it
    // covers, partial pixels weighted by coverage.
    Image out(static_cast<size_t>(dw) * dh * 3);
    const double fx = static_cast<double>(sw) / dw, fy = static_cast<double>(sh) / dh;
    for (uint32_t y = 0; y < dh; ++y)
    {
        const double y0 = y * fy, y1 = (y + 1) * fy;
        for (uint32_t x = 0; x < dw; ++x)
        {
            const double x0 = x * fx, x1 = (x + 1) * fx;
            double acc[3] = {0, 0, 0}, wsum = 0;
            for (uint32_t sy = static_cast<uint32_t>(y0); sy < std::min<double>(sh, std::ceil(y1)); ++sy)
            {
                const double wy = std::min<double>(sy + 1, y1) - std::max<double>(sy, y0);
                for (uint32_t sx = static_cast<uint32_t>(x0); sx < std::min<double>(sw, std::ceil(x1)); ++sx)
                {
                    const double wgt = wy * (std::min<double>(sx + 1, x1) - std::max<double>(sx, x0));
                    const uint8_t *p = &src[(static_cast<size_t>(sy) * sw + sx) * 3];
                    for (int c = 0; c < 3; ++c) acc[c] += wgt * p[c];
                    wsum += wgt;
                }
            }
            for (int c = 0; c < 3; ++c)
                out[(static_cast<size_t>(y) * dw + x) * 3 + c] =
                    static_cast<uint8_t>(std::lround(clampd(acc[c] / wsum, 0.0, 255.0)));
        }
    }
    return out;
}

Image analogize(const Image &src, uint32_t w, uint32_t h)
{
    // Light horizontal blur (an analog signal's limited bandwidth) and
    // gaussian noise, sigma 4 levels; fixed seed so runs compare.
    Image out(src.size());
    std::mt19937 rng(12345);
    std::normal_distribution<double> noise(0.0, 4.0);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
            {
                auto at = [&](uint32_t xx) { return src[(static_cast<size_t>(y) * w + xx) * 3 + c]; };
                const double v = 0.25 * at(x > 0 ? x - 1 : x) + 0.5 * at(x) + 0.25 * at(x + 1 < w ? x + 1 : x);
                out[(static_cast<size_t>(y) * w + x) * 3 + c] =
                    static_cast<uint8_t>(std::lround(clampd(v + noise(rng), 0.0, 255.0)));
            }
    return out;
}

// ------------------------------------------------------------- baselines
Image upNearest(const Image &src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
    return decimateNearest(src, sw, sh, dw, dh); // same mapping works both ways
}

Image upBilinear(const Image &src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
    Image out(static_cast<size_t>(dw) * dh * 3);
    for (uint32_t y = 0; y < dh; ++y)
    {
        const double fy = clampd((y + 0.5) * sh / dh - 0.5, 0.0, sh - 1.0);
        const uint32_t y0 = static_cast<uint32_t>(fy), y1 = std::min(sh - 1, y0 + 1);
        const double ty = fy - y0;
        for (uint32_t x = 0; x < dw; ++x)
        {
            const double fx = clampd((x + 0.5) * sw / dw - 0.5, 0.0, sw - 1.0);
            const uint32_t x0 = static_cast<uint32_t>(fx), x1 = std::min(sw - 1, x0 + 1);
            const double tx = fx - x0;
            for (int c = 0; c < 3; ++c)
            {
                auto p = [&](uint32_t xx, uint32_t yy) { return static_cast<double>(src[(static_cast<size_t>(yy) * sw + xx) * 3 + c]); };
                const double v = (p(x0, y0) * (1 - tx) + p(x1, y0) * tx) * (1 - ty) +
                                 (p(x0, y1) * (1 - tx) + p(x1, y1) * tx) * ty;
                out[(static_cast<size_t>(y) * dw + x) * 3 + c] = static_cast<uint8_t>(std::lround(v));
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------- metrics
double psnr(const Image &a, const Image &b)
{
    double se = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const double d = static_cast<double>(a[i]) - b[i];
        se += d * d;
    }
    const double mse = se / static_cast<double>(a.size());
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

std::vector<double> luma(const Image &img, uint32_t w, uint32_t h)
{
    std::vector<double> y(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < y.size(); ++i)
        y[i] = 0.299 * img[i * 3] + 0.587 * img[i * 3 + 1] + 0.114 * img[i * 3 + 2];
    return y;
}

double ssim(const std::vector<double> &a, const std::vector<double> &b, uint32_t w, uint32_t h)
{
    const double c1 = (0.01 * 255) * (0.01 * 255), c2 = (0.03 * 255) * (0.03 * 255);
    double sum = 0.0;
    int n = 0;
    for (uint32_t y = 0; y + 8 <= h; y += 8)
        for (uint32_t x = 0; x + 8 <= w; x += 8)
        {
            double ma = 0, mb = 0;
            for (uint32_t j = 0; j < 8; ++j)
                for (uint32_t i = 0; i < 8; ++i)
                {
                    ma += a[(y + j) * w + x + i];
                    mb += b[(y + j) * w + x + i];
                }
            ma /= 64.0;
            mb /= 64.0;
            double va = 0, vb = 0, cov = 0;
            for (uint32_t j = 0; j < 8; ++j)
                for (uint32_t i = 0; i < 8; ++i)
                {
                    const double da = a[(y + j) * w + x + i] - ma, db = b[(y + j) * w + x + i] - mb;
                    va += da * da;
                    vb += db * db;
                    cov += da * db;
                }
            va /= 63.0;
            vb /= 63.0;
            cov /= 63.0;
            sum += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
            ++n;
        }
    return n ? sum / n : 0.0;
}

double sobelEnergy(const std::vector<double> &y, uint32_t w, uint32_t h)
{
    double e = 0.0;
    for (uint32_t r = 1; r + 1 < h; ++r)
        for (uint32_t c = 1; c + 1 < w; ++c)
        {
            auto p = [&](uint32_t cc, uint32_t rr) { return y[static_cast<size_t>(rr) * w + cc]; };
            const double gx = (p(c + 1, r - 1) + 2 * p(c + 1, r) + p(c + 1, r + 1)) -
                              (p(c - 1, r - 1) + 2 * p(c - 1, r) + p(c - 1, r + 1));
            const double gy = (p(c - 1, r + 1) + 2 * p(c, r + 1) + p(c + 1, r + 1)) -
                              (p(c - 1, r - 1) + 2 * p(c, r - 1) + p(c + 1, r - 1));
            e += std::sqrt(gx * gx + gy * gy);
        }
    return e;
}

void writePPM(const std::string &path, const Image &img, uint32_t w, uint32_t h)
{
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << w << " " << h << "\n255\n";
    f.write(reinterpret_cast<const char *>(img.data()), static_cast<std::streamsize>(img.size()));
}

// -------------------------------------------------------------------- GL
GLuint uploadTexture(const Image &rgb, uint32_t w, uint32_t h)
{
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i)
    {
        rgba[i * 4 + 0] = rgb[i * 3 + 0];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba.data());
    // The pipeline hands the upscaler the source with the capture's filter
    // (LINEAR for a FrameProcessor texture).
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

Image readTexture(GLuint tex, uint32_t w, uint32_t h)
{
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    glReadPixels(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    Image rgb(static_cast<size_t>(w) * h * 3);
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i)
        for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = rgba[i * 4 + c];
    return rgb;
}

double msSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
} // namespace

int runUpscalerBench(const std::string &spec)
{
    auto kv = parseSpec(spec);
    uint32_t lw = 640, lh = 480, ow = 1920, oh = 1440, cw = 960, ch = 720;
    if (!parseSize(kv, "in", lw, lh) || !parseSize(kv, "out", ow, oh) || !parseSize(kv, "cap", cw, ch))
    {
        std::fprintf(stderr, "upscaler-bench: bad spec '%s'\n", spec.c_str());
        return 2;
    }
    const std::string dump = kv.count("dump") ? kv["dump"] : "";

    const Image truth = renderEdges(ow, oh);
    const std::vector<double> truthY = luma(truth, ow, oh);
    const double truthEdge = sobelEnergy(truthY, ow, oh);
    if (!dump.empty()) writePPM(dump + "/truth.ppm", truth, ow, oh);

    const Image native = renderEdges(lw, lh);
    const Image capture = renderEdges(cw, ch);
    struct Input
    {
        const char *name;
        Image img;
    };
    std::vector<Input> inputs = {
        {"native", native},
        {"noisy", analogize(native, lw, lh)},
        {"nearest", decimateNearest(capture, cw, ch, lw, lh)},
        {"area", decimateArea(capture, cw, ch, lw, lh)},
    };

    std::printf("UPSCALE bench: %ux%u -> %ux%u (capture %ux%u for nearest/area)\n", lw, lh, ow, oh, cw, ch);
    auto report = [&](const char *input, const char *method, const Image &out, double ms) {
        const std::vector<double> y = luma(out, ow, oh);
        std::printf("UPSCALE input=%-8s method=%-12s psnr=%6.2f ssim=%.4f edge=%.3f ms=%.2f\n", input, method,
                    psnr(out, truth), ssim(y, truthY, ow, oh), sobelEnergy(y, ow, oh) / truthEdge, ms);
        std::fflush(stdout);
        if (!dump.empty())
            writePPM(dump + "/" + input + "-" + method + ".ppm", out, ow, oh);
    };

    UpscalerStage stage;
    const UpscalerSettings::Mode modes[] = {
        UpscalerSettings::Mode::Fsr,      UpscalerSettings::Mode::FsrEasu,  UpscalerSettings::Mode::Nis,
        UpscalerSettings::Mode::Anime4kS, UpscalerSettings::Mode::Anime4kM, UpscalerSettings::Mode::DenoiseFsr,
    };
    int failures = 0;
    for (const Input &in : inputs)
    {
        if (!dump.empty()) writePPM(dump + "/" + in.name + "-input.ppm", in.img, lw, lh);
        report(in.name, "nearest", upNearest(in.img, lw, lh, ow, oh), 0.0);
        report(in.name, "bilinear", upBilinear(in.img, lw, lh, ow, oh), 0.0);

        const GLuint src = uploadTexture(in.img, lw, lh);
        for (UpscalerSettings::Mode m : modes)
        {
            UpscalerSettings s;
            s.mode = m;
            s.position = UpscalerSettings::Position::Before;
            s.target = UpscalerSettings::Target::Auto;
            std::vector<double> times;
            GLuint out = src;
            uint32_t w = lw, h = lh;
            for (int i = 0; i < 11; ++i)
            {
                w = lw;
                h = lh;
                glFinish();
                const auto t0 = std::chrono::steady_clock::now();
                out = stage.process(s, UpscalerSettings::Position::Before, src, w, h, lw, lh, ow, oh,
                                    /*fitAspect=*/false);
                glFinish();
                if (i > 0) times.push_back(msSince(t0)); // the first run pays shader compilation
            }
            if (out == src || w != ow || h != oh)
            {
                std::printf("UPSCALE input=%-8s method=%-12s FAILED (output %ux%u)\n", in.name,
                            UpscalerSettings::modeName(m), w, h);
                ++failures;
                continue;
            }
            std::sort(times.begin(), times.end());
            report(in.name, UpscalerSettings::modeName(m), readTexture(out, ow, oh), times[times.size() / 2]);
        }
        glDeleteTextures(1, &src);
    }
    stage.shutdown();
    return failures ? 1 : 0;
}
