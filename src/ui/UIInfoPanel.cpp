#include "UIInfoPanel.h"
#include "UIManager.h"
#include "UISectionHeader.h"
#include "../capture/IVideoCapture.h"
#include "../utils/TranslationManager.h"
#include <imgui.h>

#ifndef RETROCAPTURE_VERSION
#define RETROCAPTURE_VERSION "0.0.0-dev"
#endif

UIInfoPanel::UIInfoPanel(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UIInfoPanel::~UIInfoPanel()
{
}

void UIInfoPanel::render()
{
    if (!m_visible || !m_uiManager) return;

    ImGui::SetNextWindowSize(ImVec2(480, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("info.title").c_str(), &m_visible))
    {
        ImGui::End();
        return;
    }

    // Client mode (Remote source active) and host mode are two very
    // different things — same Info tab used to show 'Capture Information'
    // with the remote URL as a 'device' and an inactive 'streaming
    // status', which is meaningless to a viewer. Split them.
    const bool isClient = (m_uiManager->getSourceType() == UIManager::SourceType::Remote) &&
                          !m_uiManager->getCurrentDevice().empty();
    if (isClient)
    {
        renderRemoteInfo();
    }
    else
    {
        renderCaptureInfo();
        renderStreamingInfo();
    }
    renderSystemInfo();

    ImGui::End();
}

void UIInfoPanel::renderCaptureInfo()
{
    ui_section_header(T("info.capture").c_str());

    const std::string device = m_uiManager->getCaptureDevice();
    ImGui::Text("%s: %s", T("info.capture.device").c_str(),
                device.empty() ? "(none)" : device.c_str());
    ImGui::Text("%s: %ux%u", T("info.capture.resolution").c_str(),
                m_uiManager->getCaptureWidth(),
                m_uiManager->getCaptureHeight());
    ImGui::Text("%s: %u", T("info.capture.fps").c_str(),
                m_uiManager->getCaptureFps());
}

void UIInfoPanel::renderStreamingInfo()
{
    ui_section_header(T("info.streaming").c_str());

    bool active = m_uiManager->getStreamingActive();
    ImGui::Text("%s:", T("info.streaming.status").c_str());
    ImGui::SameLine();
    if (active)
    {
        ImGui::TextColored(ImVec4(0.40f, 0.80f, 0.40f, 1.0f), "%s",
                           T("info.streaming.active").c_str());
        std::string url = m_uiManager->getStreamUrl();
        if (!url.empty())
        {
            ImGui::Text("%s: %s", T("info.streaming.url").c_str(), url.c_str());
        }
        ImGui::Text("%s: %u", T("info.streaming.clients").c_str(),
                    m_uiManager->getStreamClientCount());
    }
    else
    {
        ImGui::TextDisabled("%s", T("info.streaming.inactive").c_str());
    }
}

void UIInfoPanel::renderRemoteInfo()
{
    ui_section_header(T("info.remote_stream").c_str());

    ImGui::Text("%s: %s", T("info.remote.host_url").c_str(),
                m_uiManager->getCurrentDevice().c_str());
    const uint32_t w = m_uiManager->getCaptureWidth();
    const uint32_t h = m_uiManager->getCaptureHeight();
    if (w > 0 && h > 0)
    {
        ImGui::Text("%s: %ux%u", T("info.remote.incoming").c_str(), w, h);
    }
    else
    {
        ImGui::TextDisabled("%s: %s", T("info.remote.incoming").c_str(),
                            T("info.remote.negotiating").c_str());
    }
    const std::string interp = m_uiManager->getRemoteInterpolation();
    ImGui::Text("%s: %s", T("info.remote.interp").c_str(),
                interp.empty() ? "linear" : interp.c_str());

    ui_section_header(T("info.connection").c_str());

    const bool hasFrames = (w > 0 && h > 0);
    if (m_uiManager->getRemoteHostLikelyOffline())
    {
        ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1.0f), "%s",
                           T("info.connection.offline").c_str());
        ImGui::TextWrapped("%s", T("info.connection.offline.hint").c_str());
    }
    else if (hasFrames)
    {
        ImGui::TextColored(ImVec4(0.40f, 0.80f, 0.40f, 1.0f), "%s",
                           T("info.connection.connected").c_str());
    }
    else
    {
        ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1.0f), "%s",
                           T("info.connection.reconnecting").c_str());
        ImGui::TextWrapped("%s", T("info.connection.reconnecting.hint").c_str());
    }
}

void UIInfoPanel::renderSystemInfo()
{
    ui_section_header(T("info.application").c_str());

    ImGui::Text("%s: %s", T("info.application.version").c_str(), RETROCAPTURE_VERSION);
    ImGui::Text("Dear ImGui: %s", ImGui::GetVersion());
}
