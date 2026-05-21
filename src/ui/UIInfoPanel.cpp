#include "UIInfoPanel.h"
#include "UIManager.h"
#include <imgui.h>

UIInfoPanel::UIInfoPanel(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UIInfoPanel::~UIInfoPanel()
{
}

void UIInfoPanel::render()
{
    if (!m_uiManager)
    {
        return;
    }

    renderCaptureInfo();
    ImGui::Separator();
    renderStreamingInfo();
    ImGui::Separator();
    renderSystemInfo();
}

void UIInfoPanel::renderCaptureInfo()
{
    ImGui::Text("Capture Information");
    ImGui::Separator();

    ImGui::Text("Device: %s", m_uiManager->getCaptureDevice().c_str());
    ImGui::Text("Resolution: %ux%u", m_uiManager->getCaptureWidth(), m_uiManager->getCaptureHeight());
    ImGui::Text("FPS: %u", m_uiManager->getCaptureFps());
}

void UIInfoPanel::renderStreamingInfo()
{
    ImGui::Text("Streaming Information");
    ImGui::Separator();

    bool active = m_uiManager->getStreamingActive();
    ImGui::Text("Status: %s", active ? "Ativo" : "Inativo");
    if (active)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "●");

        std::string url = m_uiManager->getStreamUrl();
        if (!url.empty())
        {
            ImGui::Text("URL: %s", url.c_str());
        }
        ImGui::Text("Clientes conectados: %u", m_uiManager->getStreamClientCount());
    }
    else
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "●");
    }
}

void UIInfoPanel::renderSystemInfo()
{
    ImGui::Text("Application Info");
    ImGui::Text("RetroCapture v0.1.0");
    ImGui::Text("ImGui: %s", ImGui::GetVersion());
}
