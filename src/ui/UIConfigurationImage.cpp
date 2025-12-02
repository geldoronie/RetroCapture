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
