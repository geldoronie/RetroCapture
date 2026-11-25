#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

// #251 — settings of the built-in upscaler stage (Image window > Upscaler).
// Plain data, no GL: shared by the UI, the config / capture presets, the web
// API and the render loop (which reads a copy every frame).
struct UpscalerSettings
{
    enum class Mode
    {
        Off = 0,
        Fsr,        // FSR 1: EASU + RCAS
        FsrEasu,    // FSR 1, EASU only
        Nis,        // NVIDIA Image Scaling
        Anime4kS,   // Anime4K v4 Upscale CNN x2 (S)
        Anime4kM,   // Anime4K v4 Upscale CNN x2 (M)
        DenoiseFsr, // fast bilateral, then FSR 1 with noise-aware RCAS
    };
    static constexpr int kModeCount = 7;

    enum class Target
    {
        Auto = 0, // window / output resolution
        X2,
        X3,
        X4,
        Custom,
    };
    static constexpr int kTargetCount = 5;

    enum class Position
    {
        Before = 0, // upscaler, then the user's shader chain (default)
        After,      // user's shader chain at native size, then the upscaler
    };

    Mode mode = Mode::Off;
    Target target = Target::Auto;
    int customWidth = 1920;
    int customHeight = 1080;
    float sharpness = 0.75f; // 0..1, mapped to FSR_SHARPENING / NIS_SHARPNESS
    Position position = Position::Before;

    bool active() const { return mode != Mode::Off; }
    bool usesSharpness() const
    {
        return mode == Mode::Fsr || mode == Mode::Nis || mode == Mode::DenoiseFsr;
    }

    // Preset file (relative to shaders/internal/upscalers/) of a mode.
    static const char *presetFile(Mode m)
    {
        switch (m)
        {
        case Mode::Fsr: return "fsr.glslp";
        case Mode::FsrEasu: return "fsr-easu.glslp";
        case Mode::Nis: return "nis.glslp";
        case Mode::Anime4kS: return "anime4k-cnn-x2-s.glslp";
        case Mode::Anime4kM: return "anime4k-cnn-x2-m.glslp";
        case Mode::DenoiseFsr: return "denoise+fsr.glslp";
        default: return "";
        }
    }

    // Stable names used by the config, presets and the web API.
    static const char *modeName(Mode m)
    {
        switch (m)
        {
        case Mode::Fsr: return "fsr";
        case Mode::FsrEasu: return "fsr-easu";
        case Mode::Nis: return "nis";
        case Mode::Anime4kS: return "anime4k-s";
        case Mode::Anime4kM: return "anime4k-m";
        case Mode::DenoiseFsr: return "denoise-fsr";
        default: return "off";
        }
    }
    static bool modeFromName(const std::string &s, Mode &out)
    {
        for (int i = 0; i < kModeCount; ++i)
        {
            if (s == modeName(static_cast<Mode>(i)))
            {
                out = static_cast<Mode>(i);
                return true;
            }
        }
        return false;
    }
    static const char *targetName(Target t)
    {
        switch (t)
        {
        case Target::X2: return "2x";
        case Target::X3: return "3x";
        case Target::X4: return "4x";
        case Target::Custom: return "custom";
        default: return "auto";
        }
    }
    static bool targetFromName(const std::string &s, Target &out)
    {
        for (int i = 0; i < kTargetCount; ++i)
        {
            if (s == targetName(static_cast<Target>(i)))
            {
                out = static_cast<Target>(i);
                return true;
            }
        }
        return false;
    }
    static const char *positionName(Position p) { return p == Position::After ? "after" : "before"; }
    static bool positionFromName(const std::string &s, Position &out)
    {
        if (s == "before") { out = Position::Before; return true; }
        if (s == "after") { out = Position::After; return true; }
        return false;
    }

    static int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static float clampFloat(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    void sanitize()
    {
        customWidth = clampInt(customWidth, 16, 7680);
        customHeight = clampInt(customHeight, 16, 4320);
        if (!(sharpness >= 0.0f)) sharpness = 0.0f; // also catches NaN
        sharpness = clampFloat(sharpness, 0.0f, 1.0f);
    }

    // Upscale target for a refW x refH source. autoW/H is the window or
    // output resolution; with fitAspect the Auto target keeps the source
    // aspect inside it (the window letterboxes anyway).
    void targetSize(uint32_t refW, uint32_t refH, uint32_t autoW, uint32_t autoH,
                    bool fitAspect, uint32_t &outW, uint32_t &outH) const
    {
        outW = refW;
        outH = refH;
        switch (target)
        {
        case Target::X2: outW = refW * 2; outH = refH * 2; break;
        case Target::X3: outW = refW * 3; outH = refH * 3; break;
        case Target::X4: outW = refW * 4; outH = refH * 4; break;
        case Target::Custom:
            outW = static_cast<uint32_t>(clampInt(customWidth, 16, 7680));
            outH = static_cast<uint32_t>(clampInt(customHeight, 16, 4320));
            break;
        default:
            if (autoW > 0 && autoH > 0)
            {
                outW = autoW;
                outH = autoH;
                if (fitAspect && refW > 0 && refH > 0)
                {
                    const double s = std::min(static_cast<double>(autoW) / refW,
                                              static_cast<double>(autoH) / refH);
                    outW = static_cast<uint32_t>(std::lround(refW * s));
                    outH = static_cast<uint32_t>(std::lround(refH * s));
                }
            }
            break;
        }
        // Stay inside what the pipeline handles everywhere else.
        if (outW > 7680 || outH > 4320)
        {
            const double s = std::min(7680.0 / outW, 4320.0 / outH);
            outW = static_cast<uint32_t>(outW * s);
            outH = static_cast<uint32_t>(outH * s);
        }
        if (outW == 0) outW = 1;
        if (outH == 0) outH = 1;
    }

    nlohmann::json toJson() const
    {
        return nlohmann::json{{"mode", modeName(mode)},
                              {"target", targetName(target)},
                              {"customWidth", customWidth},
                              {"customHeight", customHeight},
                              {"sharpness", sharpness},
                              {"position", positionName(position)}};
    }

    // Applies the keys present in j (unknown values are ignored). Returns
    // false and fills err when a key has an invalid value (strict = API).
    bool applyJson(const nlohmann::json &j, std::string *err = nullptr)
    {
        if (!j.is_object())
        {
            if (err) *err = "'upscaler' must be an object";
            return false;
        }
        if (j.contains("mode"))
        {
            Mode m;
            if (!j["mode"].is_string() || !modeFromName(j["mode"].get<std::string>(), m))
            {
                if (err) *err = "Invalid upscaler 'mode'. Use off, fsr, fsr-easu, nis, anime4k-s, anime4k-m or denoise-fsr";
                return false;
            }
            mode = m;
        }
        if (j.contains("target"))
        {
            Target t;
            if (!j["target"].is_string() || !targetFromName(j["target"].get<std::string>(), t))
            {
                if (err) *err = "Invalid upscaler 'target'. Use auto, 2x, 3x, 4x or custom";
                return false;
            }
            target = t;
        }
        if (j.contains("position"))
        {
            Position p;
            if (!j["position"].is_string() || !positionFromName(j["position"].get<std::string>(), p))
            {
                if (err) *err = "Invalid upscaler 'position'. Use before or after";
                return false;
            }
            position = p;
        }
        if (j.contains("customWidth") && j["customWidth"].is_number())
            customWidth = j["customWidth"].get<int>();
        if (j.contains("customHeight") && j["customHeight"].is_number())
            customHeight = j["customHeight"].get<int>();
        if (j.contains("sharpness") && j["sharpness"].is_number())
            sharpness = j["sharpness"].get<float>();
        sanitize();
        return true;
    }

    bool operator==(const UpscalerSettings &o) const
    {
        return mode == o.mode && target == o.target && customWidth == o.customWidth &&
               customHeight == o.customHeight && sharpness == o.sharpness && position == o.position;
    }
    bool operator!=(const UpscalerSettings &o) const { return !(*this == o); }
};
