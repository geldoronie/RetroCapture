#include "UpscalerStage.h"

#include "../shader/ShaderEngine.h"
#include "../utils/FilesystemCompat.h"
#include "../utils/Logger.h"
#include "../utils/Paths.h"

#include <cstdlib>
#include <vector>

UpscalerStage::UpscalerStage() = default;

UpscalerStage::~UpscalerStage()
{
    // GL objects must go while the context is alive: Application calls
    // shutdown() during its GL teardown. Nothing GL is touched here.
    if (m_engine)
    {
        m_engine.release(); // leak on purpose if shutdown() was skipped
    }
}

void UpscalerStage::shutdown()
{
    if (m_engine)
    {
        m_engine->shutdown();
        m_engine.reset();
    }
    m_loadedMode = UpscalerSettings::Mode::Off;
    m_appliedSharpness = -1.0f;
}

void UpscalerStage::setStateFrozen(bool frozen)
{
    m_stateFrozen = frozen;
    if (m_engine)
        m_engine->setStateFrozen(frozen);
}

std::string UpscalerStage::presetDir()
{
    std::vector<fs::path> candidates;
    // AppImage: RETROCAPTURE_SHADER_PATH points at .../shaders/shaders_glsl.
    if (const char *env = std::getenv("RETROCAPTURE_SHADER_PATH"))
    {
        if (*env)
            candidates.push_back(fs::path(env).parent_path() / "internal" / "upscalers");
    }
    candidates.push_back(fs::path(Paths::getReadOnlyAssetsDir()) / "shaders" / "internal" / "upscalers");
    const fs::path exeDir(Paths::getExecutableDir());
    candidates.push_back(exeDir / "shaders" / "internal" / "upscalers");
    candidates.push_back(exeDir.parent_path() / "share" / "retrocapture" / "shaders" / "internal" / "upscalers");
    candidates.push_back(exeDir.parent_path() / "Resources" / "shaders" / "internal" / "upscalers");
    for (const auto &c : candidates)
    {
        if (fs::exists(c / "fsr.glslp"))
            return c.string();
    }
    return candidates.front().string();
}

bool UpscalerStage::ensureMode(UpscalerSettings::Mode mode)
{
    if (mode == m_loadedMode && m_engine && m_engine->isShaderActive())
        return true;
    if (mode == m_failedMode)
        return false;

    if (!m_engine && !m_engineFailed)
    {
        m_engine = std::make_unique<ShaderEngine>();
        if (!m_engine->init())
        {
            LOG_ERROR("Upscaler: failed to initialise its shader engine");
            m_engine.reset();
            m_engineFailed = true;
        }
        else
        {
            m_engine->setFrameHistoryEnabled(false);
            m_engine->setStateFrozen(m_stateFrozen);
        }
    }
    if (!m_engine)
        return false;

    const std::string path = (fs::path(presetDir()) / UpscalerSettings::presetFile(mode)).string();
    m_appliedSharpness = -1.0f;
    if (!m_engine->loadPreset(path))
    {
        LOG_ERROR("Upscaler: could not load " + path + " (mode " +
                  UpscalerSettings::modeName(mode) + "); passing the image through");
        m_engine->disableShader();
        m_loadedMode = UpscalerSettings::Mode::Off;
        m_failedMode = mode;
        return false;
    }
    LOG_INFO(std::string("Upscaler: ") + UpscalerSettings::modeName(mode) + " loaded from " + path);
    m_loadedMode = mode;
    m_logCount = 0;
    m_failedMode = UpscalerSettings::Mode::Off;
    return true;
}

void UpscalerStage::applySharpness(const UpscalerSettings &s)
{
    if (!s.usesSharpness() || s.sharpness == m_appliedSharpness)
        return;
    // FSR_SHARPENING is RCAS strength in stops (0 = sharpest, 2 = softest).
    m_engine->setShaderParameter("FSR_SHARPENING", 2.0f * (1.0f - s.sharpness));
    m_engine->setShaderParameter("NIS_SHARPNESS", s.sharpness);
    m_appliedSharpness = s.sharpness;
}

GLuint UpscalerStage::process(const UpscalerSettings &s, UpscalerSettings::Position where,
                              GLuint tex, uint32_t &w, uint32_t &h,
                              uint32_t refW, uint32_t refH,
                              uint32_t autoW, uint32_t autoH, bool fitAspect)
{
    if (!runsAt(s, where) || tex == 0 || w == 0 || h == 0)
        return tex;
    if (!ensureMode(s.mode))
        return tex;
    applySharpness(s);

    uint32_t tw = 0, th = 0;
    s.targetSize(refW ? refW : w, refH ? refH : h, autoW, autoH, fitAspect, tw, th);
    m_engine->setViewport(tw, th);

    const GLuint out = m_engine->applyShader(tex, w, h);
    if (out == 0 || out == tex)
        return tex;
    const uint32_t ow = m_engine->getOutputWidth();
    const uint32_t oh = m_engine->getOutputHeight();
    if (ow == 0 || oh == 0)
        return tex;

    if (m_logCount++ < 3)
    {
        LOG_INFO(std::string("Upscaler: ") + UpscalerSettings::modeName(s.mode) + " " +
                 std::to_string(w) + "x" + std::to_string(h) + " -> " +
                 std::to_string(ow) + "x" + std::to_string(oh) + " (" +
                 UpscalerSettings::positionName(s.position) + " the shader chain)");
    }
    w = ow;
    h = oh;
    return out;
}
