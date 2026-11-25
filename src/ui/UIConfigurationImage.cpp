#include "UIConfigurationImage.h"
#include "UIManager.h"
#include "UISectionHeader.h"
#include "../utils/TranslationManager.h"
#include "../processing/FrameGenSettings.h"
#include <imgui.h>
#include <algorithm>

UIConfigurationImage::UIConfigurationImage(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UIConfigurationImage::~UIConfigurationImage()
{
}

void UIConfigurationImage::render()
{
    if (!m_visible || !m_uiManager) return;

    ImGui::SetNextWindowSize(ImVec2(520, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("image.title").c_str(), &m_visible))
    {
        ImGui::End();
        return;
    }

    ui_section_header("Image adjustments",
                      "Post-shader, pre-output transforms that only "
                      "affect what you see locally — they're not "
                      "embedded in the stream or recording.");

    renderBrightnessContrast();
    renderSourceColor();
    renderFrameGeneration();
    renderUpscaler();
    renderOutputResolution();
    renderAspectRatio();
    renderFullscreen();
    renderMonitorSelection();
    renderFpsOverlay();

    ImGui::End();
}

void UIConfigurationImage::renderBrightnessContrast()
{
    // Brightness
    float brightness = m_uiManager->getBrightness();
    if (ImGui::SliderFloat("Brightness", &brightness, 0.0f, 2.0f, "%.2f"))
    {
        m_uiManager->setBrightness(brightness);
        m_uiManager->saveConfig();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##brightness"))
    {
        m_uiManager->setBrightness(1.0f);
    }

    // Contrast
    float contrast = m_uiManager->getContrast();
    if (ImGui::SliderFloat("Contrast", &contrast, 0.0f, 5.0f, "%.2f"))
    {
        m_uiManager->setContrast(contrast);
        m_uiManager->saveConfig();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##contrast"))
    {
        m_uiManager->setContrast(1.0f);
    }
}

void UIConfigurationImage::renderSourceColor()
{
    // #228 — applied to the captured frame before the shader chain, so the
    // shaders and every output (stream, recording, virtual camera, /raw)
    // see the corrected image.
    ui_section_header("Source color",
                      "Applied to the captured frame before the shader "
                      "chain; affects the preview and every output.");

    static const char *kRanges[] = {"Auto", "Limited (16-235)", "Full (0-255)"};
    int range = m_uiManager->getColorRange();
    if (ImGui::Combo("Color range", &range, kRanges, 3))
    {
        m_uiManager->setColorRange(range);
        m_uiManager->saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Levels of the incoming signal. Pick Limited if "
                          "blacks look grey (washed out), Full if shadows "
                          "and highlights look crushed. Auto uses what the "
                          "device reports, else YUV = limited, RGB = full.");
    }

    {
        static const char *filters[] = {"Auto", "Nearest (sharp pixels)", "Area average"};
        int filter = m_uiManager->getSourceDownscaleFilter();
        ImGui::PushItemWidth(200);
        if (ImGui::Combo("Source downscale", &filter, filters, IM_ARRAYSIZE(filters)))
        {
            m_uiManager->setSourceDownscaleFilter(filter);
            m_uiManager->saveConfig();
        }
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Used when the capture resolution you picked is smaller than what the\n"
                              "device delivers (e.g. 640x480 from a 1280x720 grabber).\n"
                              "Nearest keeps hard pixel edges but drops lines on non-whole ratios.\n"
                              "Area average keeps every line and averages analog noise down.\n"
                              "Auto: Nearest for whole ratios (2x, 3x), else Area average.");
        }
    }

    bool expandAfter = m_uiManager->getExpandLimitedAfterDecode();
    if (ImGui::Checkbox("Expand limited range after decode", &expandAfter))
    {
        m_uiManager->setExpandLimitedAfterDecode(expandAfter);
        m_uiManager->saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Stretches 16-235 to 0-255 after the YUV->RGB decode, "
                          "independent of Color range. For grabbers that put "
                          "limited-range RGB inside a limited-range YUV signal "
                          "(blacks still grey after picking the right range).");
    }

    float saturation = m_uiManager->getSaturation();
    if (ImGui::SliderFloat("Saturation", &saturation, 0.0f, 3.0f, "%.2f"))
    {
        m_uiManager->setSaturation(saturation);
        m_uiManager->saveConfig();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##saturation"))
    {
        m_uiManager->setSaturation(1.0f);
        m_uiManager->saveConfig();
    }

    float gamma = m_uiManager->getGamma();
    if (ImGui::SliderFloat("Gamma", &gamma, 0.2f, 3.0f, "%.2f"))
    {
        m_uiManager->setGamma(gamma);
        m_uiManager->saveConfig();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##gamma"))
    {
        m_uiManager->setGamma(1.0f);
        m_uiManager->saveConfig();
    }

    // #268 — temporal denoise (analog noise, chroma speckle, shimmer).
    float denoise = m_uiManager->getTemporalDenoise();
    if (ImGui::SliderFloat("Temporal denoise", &denoise, 0.0f, 1.0f, denoise <= 0.0f ? "Off" : "%.2f"))
    {
        m_uiManager->setTemporalDenoise(denoise);
        m_uiManager->saveConfig();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Averages still areas over several frames and leaves moving ones alone:\n"
                          "cleans the noise and shimmer of analog consoles through a converter.\n"
                          "No added latency. Higher = stronger; too high can smear slow motion.\n"
                          "Runs before the shader chain and frame generation; /raw gets it too.");
    ImGui::SameLine();
    if (ImGui::Button("Off##denoise"))
    {
        m_uiManager->setTemporalDenoise(0.0f);
        m_uiManager->saveConfig();
    }

    if (ImGui::Button("Reset color to defaults"))
    {
        m_uiManager->setBrightness(1.0f);
        m_uiManager->setContrast(1.0f);
        m_uiManager->setSaturation(1.0f);
        m_uiManager->setGamma(1.0f);
        m_uiManager->setColorRange(0);
        m_uiManager->setExpandLimitedAfterDecode(false);
        m_uiManager->saveConfig();
    }
}

// #251 — built-in upscaler stage (FSR 1 / NIS / Anime4K), run in its own
// shader engine before or after the user's shader chain.
void UIConfigurationImage::renderUpscaler()
{
    ui_section_header("Upscaler",
                      "Built-in spatial upscaler. It runs in the pipeline "
                      "before or after your shader preset, so both work "
                      "together. Applies to the window, stream, recording "
                      "and virtual camera; /raw gets the upscale (never the "
                      "shader) unless 'Upscale /raw' is off.");

    UpscalerSettings up = m_uiManager->getUpscalerSettings();
    bool changed = false;

    const char *modeItems[] = {"Off", "FSR 1", "FSR 1 (EASU only)", "NVIDIA NIS",
                               "Anime4K S", "Anime4K M", "Denoise + FSR"};
    int mode = static_cast<int>(up.mode);
    ImGui::PushItemWidth(220);
    if (ImGui::Combo("Mode##upscaler", &mode, modeItems, UpscalerSettings::kModeCount))
    {
        up.mode = static_cast<UpscalerSettings::Mode>(mode);
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("FSR 1: edge-adaptive upscale + sharpening, general purpose.\n"
                          "NIS: NVIDIA's directional scaler with adaptive sharpening.\n"
                          "Anime4K: CNN for 2D / anime / pixel art (a fixed 2x network).\n"
                          "Denoise + FSR: bilateral denoise first, for noisy analog captures.");

    const bool on = up.active();
    if (!on) ImGui::BeginDisabled();

    const char *targetItems[] = {"Auto (window / output size)", "2x", "3x", "4x", "Custom"};
    int target = static_cast<int>(up.target);
    if (ImGui::Combo("Target##upscaler", &target, targetItems, UpscalerSettings::kTargetCount))
    {
        up.target = static_cast<UpscalerSettings::Target>(target);
        changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Auto: the output resolution when one is set, else the window size\n"
                          "(keeping the source aspect when Maintain Aspect Ratio is on).\n"
                          "2x / 3x / 4x: multiples of the source size.");
    ImGui::PopItemWidth();
    if (up.target == UpscalerSettings::Target::Custom)
    {
        ImGui::PushItemWidth(120);
        int cw = up.customWidth, ch = up.customHeight;
        if (ImGui::InputInt("Width##upscaler", &cw, 16, 256))
        {
            up.customWidth = cw;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::InputInt("Height##upscaler", &ch, 16, 256))
        {
            up.customHeight = ch;
            changed = true;
        }
        ImGui::PopItemWidth();
    }

    const bool sharpOff = on && !up.usesSharpness();
    if (sharpOff) ImGui::BeginDisabled();
    ImGui::PushItemWidth(220);
    float sharp = up.sharpness;
    if (ImGui::SliderFloat("Sharpness##upscaler", &sharp, 0.0f, 1.0f, "%.2f"))
    {
        up.sharpness = sharp;
        changed = true;
    }
    ImGui::PopItemWidth();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("FSR 1 / Denoise + FSR: RCAS sharpening (FSR_SHARPENING = 2 x (1 - value) stops).\n"
                          "NIS: NIS_SHARPNESS. Not used by EASU only and Anime4K.");
    if (sharpOff) ImGui::EndDisabled();

    ImGui::TextUnformatted("Position:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Before the shader chain##upscaler",
                           up.position == UpscalerSettings::Position::Before))
    {
        up.position = UpscalerSettings::Position::Before;
        changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Your shader preset runs on the upscaled image.");
    ImGui::SameLine();
    if (ImGui::RadioButton("After##upscaler", up.position == UpscalerSettings::Position::After))
    {
        up.position = UpscalerSettings::Position::After;
        changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("CRT and pixel-art shaders expect the native resolution: run them\n"
                          "first and upscale their output. With a CRT shader, 'After' keeps\n"
                          "the scanline density right (one scanline per source line).\n"
                          "The chain then renders at the largest whole multiple of the source\n"
                          "that fits the target, and the upscaler scales that to the target.");

    // #264 — /raw is the picture a remote RetroCapture applies its own shader
    // to; it can also apply its own upscaler.
    bool rawUp = up.raw;
    if (ImGui::Checkbox("Upscale /raw (remote clients)##upscaler", &rawUp))
    {
        up.raw = rawUp;
        changed = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Off: /raw sends the native picture and the remote client applies its\n"
                          "own upscaler: less bandwidth and less decode work on the client.\n"
                          "/raw never carries your shader. With the upscaler after the shader\n"
                          "chain, /raw is always the native picture.");

    if (!on) ImGui::EndDisabled();

    if (on && m_uiManager->getUpscalerIdle())
    {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.30f, 1.0f),
                           "Idle: the target is not larger than the source, so there is nothing to "
                           "upscale (e.g. a 1080p capture in a smaller window). Pick a 2x-4x or "
                           "custom target, or enlarge the window, to use it.");
        ImGui::PopTextWrapPos();
    }

    if (ImGui::Button("Reset##upscaler"))
    {
        up = UpscalerSettings();
        changed = true;
    }

    if (changed)
    {
        m_uiManager->setUpscalerSettings(up);
        m_uiManager->saveConfig();
    }
}

void UIConfigurationImage::renderAspectRatio()
{
    OutputGeometry geom = m_uiManager->getOutputGeometry();
    bool geomChanged = false;

    // Legacy toggle: governs the Auto aspect without integer scaling
    // (off = stretch to the window). An explicit aspect or integer
    // scaling always letterboxes.
    bool maintainAspect = m_uiManager->getMaintainAspect();
    if (ImGui::Checkbox("Maintain Aspect Ratio", &maintainAspect))
    {
        m_uiManager->setMaintainAspect(maintainAspect);
        m_uiManager->saveConfig();
    }
    if (geom.forcesAspect())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(implied by the geometry below)");
    }

    // #227 — output geometry.
    ui_section_header("Geometry",
                      "Aspect and rotation/mirror also apply to the stream, "
                      "recording and virtual camera (fitted into their frame). "
                      "Integer scaling only affects this window.");

    const char *scaleItems[] = {"Off (fit)", "Integer (uniform NxN)",
                                "Integer + fill (per-axis NxM)"};
    int scale = static_cast<int>(geom.integerScale);
    ImGui::PushItemWidth(260);
    if (ImGui::Combo("Integer scaling", &scale, scaleItems, 3))
    {
        geom.integerScale = static_cast<OutputGeometry::IntegerScale>(scale);
        geomChanged = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Integer: same whole factor on both axes (1x, 2x, 3x...), "
                          "source pixel aspect, black borders.\n"
                          "Integer + fill: whole factors per axis (e.g. 4x by 3x) chosen "
                          "to best match the aspect ratio below while fitting the window.\n"
                          "Falls back to fit when the window is smaller than 1x.");

    const char *aspectItems[] = {"Auto (source)", "4:3", "8:7 (NES/SNES square pixels)",
                                 "16:9", "10:9 (Game Boy)", "3:2 (GBA)", "Custom"};
    int aspect = static_cast<int>(geom.aspect);
    if (ImGui::Combo("Aspect ratio", &aspect, aspectItems, 7))
    {
        geom.aspect = static_cast<OutputGeometry::Aspect>(aspect);
        geomChanged = true;
    }
    ImGui::PopItemWidth();
    if (geom.aspect == OutputGeometry::Aspect::Custom)
    {
        ImGui::PushItemWidth(80);
        float cw = geom.customAspectW;
        float ch = geom.customAspectH;
        if (ImGui::InputFloat("##aspectW", &cw, 0.0f, 0.0f, "%.3g"))
        {
            geom.customAspectW = std::max(0.01f, cw);
            geomChanged = true;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(":");
        ImGui::SameLine();
        if (ImGui::InputFloat("##aspectH", &ch, 0.0f, 0.0f, "%.3g"))
        {
            geom.customAspectH = std::max(0.01f, ch);
            geomChanged = true;
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        ImGui::TextUnformatted("Custom (W:H)");
    }

    ImGui::TextUnformatted("Rotation:");
    const int rotations[] = {0, 90, 180, 270};
    const char *rotLabels[] = {"0##rot", "90##rot", "180##rot", "270##rot"};
    for (int i = 0; i < 4; ++i)
    {
        ImGui::SameLine();
        if (ImGui::RadioButton(rotLabels[i], geom.rotation == rotations[i]))
        {
            geom.rotation = rotations[i];
            geomChanged = true;
        }
    }
    if (ImGui::Checkbox("Mirror horizontally", &geom.flipH))
        geomChanged = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Mirror vertically", &geom.flipV))
        geomChanged = true;
    ImGui::SameLine();
    if (ImGui::Button("Reset##geometry"))
    {
        geom = OutputGeometry();
        geomChanged = true;
    }

    if (geomChanged)
        m_uiManager->setOutputGeometry(geom); // saves the config
}

void UIConfigurationImage::renderFullscreen()
{
    bool fullscreen = m_uiManager->getFullscreen();
    if (ImGui::Checkbox("Fullscreen", &fullscreen))
    {
        m_uiManager->setFullscreen(fullscreen);
        m_uiManager->saveConfig();
    }
}

void UIConfigurationImage::renderOutputResolution()
{
    ui_section_header("Output resolution",
                      "Applied after the shader, before stretching to "
                      "the window. 0 = automatic (use source).");
    
    int outputWidth = static_cast<int>(m_uiManager->getOutputWidth());
    int outputHeight = static_cast<int>(m_uiManager->getOutputHeight());
    
    ImGui::PushItemWidth(120);
    bool changed = false;
    
    if (ImGui::InputInt("Width##output", &outputWidth, 32, 256))
    {
        outputWidth = std::max(0, outputWidth);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::InputInt("Height##output", &outputHeight, 32, 256))
    {
        outputHeight = std::max(0, outputHeight);
        changed = true;
    }
    ImGui::PopItemWidth();
    
    if (changed)
    {
        m_uiManager->setOutputResolution(static_cast<uint32_t>(outputWidth), static_cast<uint32_t>(outputHeight));
        m_uiManager->saveConfig();
    }
    
    ImGui::SameLine();
    if (ImGui::Button("Reset##output"))
    {
        m_uiManager->setOutputResolution(0, 0);
        m_uiManager->saveConfig();
    }
    
    // Botões rápidos para resoluções comuns
    ImGui::Text("Quick Presets:");
    if (ImGui::Button("1280x720##output"))
    {
        m_uiManager->setOutputResolution(1280, 720);
        m_uiManager->saveConfig();
    }
    ImGui::SameLine();
    if (ImGui::Button("1920x1080##output"))
    {
        m_uiManager->setOutputResolution(1920, 1080);
        m_uiManager->saveConfig();
    }
    ImGui::SameLine();
    if (ImGui::Button("Auto##output"))
    {
        m_uiManager->setOutputResolution(0, 0);
        m_uiManager->saveConfig();
    }
}

void UIConfigurationImage::renderMonitorSelection()
{
    ui_section_header("Monitor",
                      "Which display fullscreen mode targets.");
    bool fullscreen = m_uiManager->getFullscreen();
    if (!fullscreen)
    {
        ImGui::TextDisabled("(only used in fullscreen mode)");
    }
    else
    {
        ImGui::TextDisabled("(-1 = primary monitor, 0+ = specific monitor)");
    }
    int monitorIndex = m_uiManager->getMonitorIndex();
    ImGui::PushItemWidth(100);
    if (ImGui::InputInt("##monitor", &monitorIndex, 1, 5))
    {
        monitorIndex = std::max(-1, monitorIndex); // Não permitir valores negativos menores que -1
        m_uiManager->setMonitorIndex(monitorIndex);
        m_uiManager->saveConfig();
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Reset##monitor"))
    {
        m_uiManager->setMonitorIndex(-1);
    }
}

void UIConfigurationImage::renderFpsOverlay()
{
    ui_section_header("FPS counter",
                      "On-screen capture / render / encoder rates (F9). "
                      "Drawn on this window only, never in the stream "
                      "or recording.");

    bool enabled = m_uiManager->getFpsOverlayEnabled();
    if (ImGui::Checkbox("Show FPS counter", &enabled))
    {
        m_uiManager->setFpsOverlayEnabled(enabled);
    }

    if (!enabled) ImGui::BeginDisabled();

    static const char *corners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
    int corner = m_uiManager->getFpsOverlayCorner();
    ImGui::PushItemWidth(160);
    if (ImGui::Combo("Corner##fps", &corner, corners, IM_ARRAYSIZE(corners)))
    {
        m_uiManager->setFpsOverlayCorner(corner);
    }
    ImGui::PopItemWidth();

    bool compact = m_uiManager->getFpsOverlayCompact();
    if (ImGui::Checkbox("Compact (one line)##fps", &compact))
    {
        m_uiManager->setFpsOverlayCompact(compact);
    }
    ImGui::SameLine();
    if (compact) ImGui::BeginDisabled();
    bool graph = m_uiManager->getFpsOverlayGraph();
    if (ImGui::Checkbox("Frame-time graph##fps", &graph))
    {
        m_uiManager->setFpsOverlayGraph(graph);
    }
    if (compact) ImGui::EndDisabled();

    if (!enabled) ImGui::EndDisabled();
}

// #248 — what the RIFE backend is doing (it starts with frame generation).
static void renderRifeStatus(const UIManager::FrameGenStatus &st)
{
    const ImVec4 warn(0.95f, 0.80f, 0.30f, 1.0f);
    ImGui::PushTextWrapPos(0.0f);
    if (!st.rifeCompiled)
        ImGui::TextColored(warn, "RIFE: not in this build (no ncnn/Vulkan). Using Motion.");
    else if (st.rifeState == 3)
        ImGui::TextColored(warn, "RIFE unavailable: %s. Using Motion.", st.rifeReason.c_str());
    else if (st.rifeState == 1)
        ImGui::TextDisabled("RIFE: starting Vulkan / loading the model... (Motion meanwhile)");
    else if (st.rifeState == 2)
    {
        ImGui::TextDisabled("RIFE on %s at %ux%u: %.1f ms/frame, %llu frames, %llu late (shown with Motion)",
                            st.rifeDevice.c_str(), st.rifeWidth, st.rifeHeight, st.rifeInferMs,
                            static_cast<unsigned long long>(st.rifeDelivered),
                            static_cast<unsigned long long>(st.rifeLate));
    }
    else
        ImGui::TextDisabled("RIFE starts with frame generation (needs a Vulkan GPU).");
    ImGui::PopTextWrapPos();
}

void UIConfigurationImage::renderFrameGeneration()
{
    // #247 — output clock + GPU motion-compensated interpolation.
    ui_section_header("Frame generation",
                      "Synthesises in-between frames from the source "
                      "(e.g. 30 -> 60 fps, or 60 fps on a 120/144 Hz "
                      "display). Runs before the shader chain, so shaders "
                      "process every output frame. Needs the next source "
                      "frame: adds up to one source frame of latency.");

    FrameGenSettings fg = m_uiManager->getFrameGenSettings();
    const UIManager::FrameGenStatus st = m_uiManager->getFrameGenStatus();
    const bool remote = m_uiManager->getSourceType() == UIManager::SourceType::Remote;
    bool changed = false;

    if (remote)
    {
        ImGui::TextWrapped("Remote source: pick \"Motion\" in Remote -> Connect to "
                           "Remote... -> display interpolation to generate frames "
                           "on this window.");
        ImGui::BeginDisabled();
    }

    static const char *modes[] = {"Off", "2x source rate", "3x source rate", "4x source rate",
                                  "Display refresh (window only)", "Match capture rate (fill repeats)"};
    int mode = fg.mode;
    ImGui::PushItemWidth(240);
    if (ImGui::Combo("Mode##framegen", &mode, modes, IM_ARRAYSIZE(modes)))
    {
        fg.mode = mode;
        changed = true;
    }
    static const char *methods[] = {"Blend (cheap, ghosts on motion)", "Motion (GPU optical flow)",
                                    "RIFE (neural, Vulkan; best quality)"};
    int method = fg.method;
    if (ImGui::Combo("Method##framegen", &method, methods, IM_ARRAYSIZE(methods)))
    {
        fg.method = method;
        changed = true;
    }
    if (fg.method == FrameGenSettings::RIFE)
    {
        // #260 — the network's size: its cost grows with the pixel count.
        static const char *scales[] = {"Auto (reduce above 720p)", "Full (native size)",
                                       "Fast (always ~720p)"};
        int scale = fg.rifeScale;
        if (ImGui::Combo("RIFE resolution##framegen", &scale, scales, IM_ARRAYSIZE(scales)))
        {
            fg.rifeScale = scale;
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Above 720p the network runs on a ~1280x720 copy and its frame is scaled\n"
                              "back up on the GPU: 1080p60 keeps up instead of stuttering. Real frames\n"
                              "stay native; generated ones are a little softer. Full keeps the native\n"
                              "size (needs a fast GPU above 720p).");
    }
    ImGui::PopItemWidth();
    if (ImGui::Checkbox("Keep static regions still (HUD, borders)##framegen", &fg.protectStatic))
        changed = true;
    if (ImGui::Checkbox("Skip repeated frames (game below the capture rate)##framegen", &fg.skipRepeats))
        changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Many games draw 30 or 20 fps on a 60 Hz signal: the capture repeats each\n"
                          "picture 2-3 times. With this on, repeats are not source frames, so\n"
                          "interpolation runs between the game's own frames.");

    const bool display = fg.mode == FrameGenSettings::Display;
    ImGui::TextUnformatted("Generated frames go to:");
    if (ImGui::Checkbox("Window##framegen", &fg.window)) changed = true;
    ImGui::SameLine();
    if (display) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Stream##framegen", &fg.stream)) changed = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Recording##framegen", &fg.record)) changed = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Virtual camera##framegen", &fg.virtcam)) changed = true;
    if (display) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("An output switched off keeps the real frames, with no added latency.");

    if (remote) ImGui::EndDisabled();

    if (changed)
        m_uiManager->setFrameGenSettings(fg);

    if (!st.supported)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f),
                           "Not available on this GPU (needs OpenGL 3.3 / GLES 3.0).");
    }
    else if (!st.motionSupported && fg.method == FrameGenSettings::Motion)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.30f, 1.0f),
                           "This GPU has no float render targets: using Blend.");
    }
    if (fg.method == FrameGenSettings::RIFE)
        renderRifeStatus(st);
    if (st.active)
    {
        if (fg.skipRepeats && st.captureFps > 0.0f && st.sourceFps > 0.0f &&
            st.sourceFps < st.captureFps * 0.9f)
            ImGui::Text("Game %.2f fps on a %.2f fps capture (repeats skipped)", st.sourceFps, st.captureFps);
        if (st.outputFps > 0.0f)
            ImGui::Text("Source %.2f fps -> output %.2f fps, +%.1f ms latency", st.sourceFps, st.outputFps,
                        st.addedLatencyMs);
        else
            ImGui::Text("Source %.2f fps -> one frame per refresh, +%.1f ms latency", st.sourceFps,
                        st.addedLatencyMs);
    }
    ImGui::PushTextWrapPos(0.0f);
    if (fg.mode != FrameGenSettings::Off && !display && (fg.stream || fg.record))
    {
        ImGui::TextDisabled("Stream / recording carry the generated frames their FPS allows: "
                            "set them to the output rate (e.g. 60 for 30 fps at 2x).");
    }
    if (fg.mode == FrameGenSettings::Fill && !fg.skipRepeats)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.30f, 1.0f),
                           "Match capture rate needs \"Skip repeated frames\": without it every capture "
                           "is a new frame and there is nothing to fill.");
    }
    if (fg.method == FrameGenSettings::RIFE && st.active && st.sourceHeight > 720)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.30f, 1.0f),
                           "RIFE above 720p (%ux%u) may not keep up at 60 fps and stutter; "
                           "Motion stays smooth at this size.",
                           st.sourceWidth, st.sourceHeight);
    }
    if (fg.mode == FrameGenSettings::Fill)
    {
        ImGui::TextDisabled("Match capture rate: each game frame gets as many slots as the capture "
                            "repeats it (30 fps game on 60 Hz -> 2, 20 fps -> 3); a game at the "
                            "capture rate passes through with no added latency.");
    }
    if (display)
    {
        ImGui::TextDisabled("Display mode follows the window refresh (vsync while focused); "
                            "outputs keep the real frames.");
    }
    ImGui::PopTextWrapPos();
}
