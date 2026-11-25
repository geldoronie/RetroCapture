#include "UIConfigurationImage.h"
#include "UIManager.h"
#include "UISectionHeader.h"
#include "../utils/TranslationManager.h"
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
