#include "UIConfigurationImage.h"
#include "UIManager.h"
#include <imgui.h>

UIConfigurationImage::UIConfigurationImage(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UIConfigurationImage::~UIConfigurationImage()
{
}

void UIConfigurationImage::render()
{
    if (!m_uiManager)
    {
        return;
    }

    ImGui::Text("Image Adjustments");
    ImGui::Separator();

    renderBrightnessContrast();
    ImGui::Separator();
    renderOutputResolution();
    ImGui::Separator();
    renderAspectRatio();
    renderFullscreen();
    ImGui::Separator();
    renderMonitorSelection();
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

void UIConfigurationImage::renderAspectRatio()
{
    bool maintainAspect = m_uiManager->getMaintainAspect();
    if (ImGui::Checkbox("Maintain Aspect Ratio", &maintainAspect))
    {
        m_uiManager->setMaintainAspect(maintainAspect);
        m_uiManager->saveConfig();
    }
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
    ImGui::Text("Output Resolution");
    ImGui::TextDisabled("(Applied after shader, before stretching to window)");
    ImGui::TextDisabled("(0 = automatic, use source resolution)");
    
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
    ImGui::Text("Monitor Index:");
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
