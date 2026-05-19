#include "UIInfoPanel.h"
#include "UIManager.h"
#include "../capture/IVideoCapture.h"
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
    if (!m_uiManager)
    {
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
        ImGui::Separator();
        renderStreamingInfo();
    }
    ImGui::Separator();
    renderSystemInfo();
}

void UIInfoPanel::renderCaptureInfo()
{
    ImGui::Text("Capture");
    ImGui::Separator();

    const std::string device = m_uiManager->getCaptureDevice();
    ImGui::Text("Device: %s", device.empty() ? "(none)" : device.c_str());
    ImGui::Text("Resolution: %ux%u",
                m_uiManager->getCaptureWidth(),
                m_uiManager->getCaptureHeight());
    ImGui::Text("FPS: %u", m_uiManager->getCaptureFps());
}

void UIInfoPanel::renderStreamingInfo()
{
    ImGui::Text("Streaming");
    ImGui::Separator();

    bool active = m_uiManager->getStreamingActive();
    ImGui::Text("Status:");
    ImGui::SameLine();
    if (active)
    {
        ImGui::TextColored(ImVec4(0.40f, 0.80f, 0.40f, 1.0f), "active");
        std::string url = m_uiManager->getStreamUrl();
        if (!url.empty())
        {
            ImGui::Text("URL: %s", url.c_str());
        }
        ImGui::Text("Connected clients: %u", m_uiManager->getStreamClientCount());
    }
    else
    {
        ImGui::TextDisabled("inactive");
    }
}

void UIInfoPanel::renderRemoteInfo()
{
    ImGui::Text("Remote Stream");
    ImGui::Separator();

    ImGui::Text("Host URL: %s", m_uiManager->getCurrentDevice().c_str());
    const uint32_t w = m_uiManager->getCaptureWidth();
    const uint32_t h = m_uiManager->getCaptureHeight();
    if (w > 0 && h > 0)
    {
        ImGui::Text("Incoming resolution: %ux%u", w, h);
    }
    else
    {
        ImGui::TextDisabled("Incoming resolution: (negotiating)");
    }
    const std::string interp = m_uiManager->getRemoteInterpolation();
    ImGui::Text("Display interpolation: %s",
                interp.empty() ? "linear" : interp.c_str());

    ImGui::Spacing();
    ImGui::Text("Connection");
    ImGui::Separator();

    // UIManager::getCapture() is null in Remote mode (Application
    // passes nullptr to setCaptureControls so the V4L2/DS-specific
    // hardware controls UI hides itself). So we can't reach the
    // VideoCaptureRemote through that path — Application mirrors the
    // offline flag onto UIManager every frame instead.
    //
    // 'Connected' here means we've received at least one frame, which
    // is what getCaptureWidth/Height > 0 already signals (it's the
    // same heuristic UIRemoteConnection's footer uses). 'Reconnecting'
    // means we're armed but haven't decoded a frame yet. 'Host likely
    // offline' is the long-failure hint from #58 — it can fire while
    // Connected if a previously-good stream just dropped, so it takes
    // priority over the connected indicator.
    const bool hasFrames = (w > 0 && h > 0);
    if (m_uiManager->getRemoteHostLikelyOffline())
    {
        ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1.0f), "Host likely offline");
        ImGui::TextWrapped("The client is still retrying in the background. "
                           "Disconnect and reconnect from the Remote menu "
                           "to retry immediately.");
    }
    else if (hasFrames)
    {
        ImGui::TextColored(ImVec4(0.40f, 0.80f, 0.40f, 1.0f), "Connected");
    }
    else
    {
        ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1.0f), "Reconnecting...");
        ImGui::TextWrapped("Waiting for the host's first frame. "
                           "Reconnect attempts back off up to 60 s "
                           "between tries.");
    }
}

void UIInfoPanel::renderSystemInfo()
{
    ImGui::Text("Application");
    ImGui::Separator();

    ImGui::Text("RetroCapture: %s", RETROCAPTURE_VERSION);
    ImGui::Text("Dear ImGui: %s", ImGui::GetVersion());
}
