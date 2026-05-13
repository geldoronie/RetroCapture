#include "UIRemoteConnection.h"
#include "UIManager.h"
#include "../capture/IVideoCapture.h"
#include <imgui.h>
#include <cstring>

UIRemoteConnection::UIRemoteConnection(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UIRemoteConnection::~UIRemoteConnection() = default;

void UIRemoteConnection::setVisible(bool visible)
{
    m_visible = visible;
}

void UIRemoteConnection::render()
{
    if (!m_visible || !m_uiManager) return;

    if (!m_urlSeeded)
    {
        std::string current = m_uiManager->getCurrentDevice();
        if (current.empty()) current = "http://localhost:8080";
        std::strncpy(m_urlBuffer, current.c_str(), sizeof(m_urlBuffer) - 1);
        m_urlBuffer[sizeof(m_urlBuffer) - 1] = '\0';
        m_urlSeeded = true;
    }

    ImGui::SetNextWindowSize(ImVec2(520, 320), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Connect to Remote", &m_visible))
    {
        ImGui::TextWrapped(
            "Consume a remote RetroCapture stream. The client decodes the "
            "host's /raw feed and mirrors its shader pipeline via /meta.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Remote base URL");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##remoteUrl", m_urlBuffer, sizeof(m_urlBuffer));

        ImGui::Spacing();

        // Interpolation mode dropdown — same options the Source-tab path
        // used to expose. Lives here now so all the Remote-mode controls
        // are co-located.
        const char *modes[]      = {"linear", "nearest", "off"};
        const char *modeLabels[] = {
            "Linear (smooth, slight ghosting)",
            "Nearest (clean frames, may stutter)",
            "Off (strict PTS, may stutter)"
        };
        std::string currentMode = m_uiManager->getRemoteInterpolation();
        int currentModeIndex = 0;
        for (int i = 0; i < 3; ++i)
        {
            if (currentMode == modes[i]) { currentModeIndex = i; break; }
        }
        ImGui::Text("Display interpolation");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##remoteInterp", &currentModeIndex, modeLabels, 3))
        {
            m_uiManager->triggerRemoteInterpolationChange(modes[currentModeIndex]);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Linear: blend prev+next por refresh — fluido, leve fantasma em movimento rápido.\n"
                              "Nearest: frame mais próximo no tempo — limpo, com 3:2 pulldown em ratio não-inteiro.\n"
                              "Off: estritamente espera o PTS — comportamento simples, sem ghosting nem suavização.");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const std::string currentDevice = m_uiManager->getCurrentDevice();
        const bool connected = !currentDevice.empty() &&
                               (m_capture && m_capture->isOpen());

        if (!connected)
        {
            if (ImGui::Button("Connect", ImVec2(120, 0)))
            {
                std::string url(m_urlBuffer);
                // Strip a trailing slash so the appended /raw and /meta land
                // cleanly down in VideoCaptureRemote / RemoteMetaSync.
                while (!url.empty() && url.back() == '/') url.pop_back();
                if (!url.empty())
                {
                    // setCurrentDevice fires m_onDeviceChanged, Application's
                    // connect-to-remote handler. Also flips Source to Remote
                    // so the existing remote-capture path takes over.
                    m_uiManager->setSourceType(UIManager::SourceType::Remote);
                    m_uiManager->triggerSourceTypeChange(UIManager::SourceType::Remote);
                    m_uiManager->setCurrentDevice(url);
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("not connected");
        }
        else
        {
            if (ImGui::Button("Disconnect", ImVec2(120, 0)))
            {
                m_uiManager->setCurrentDevice("");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("connected to %s", currentDevice.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextDisabled("Status");
        if (m_capture && m_capture->isOpen())
        {
            ImGui::Text("Stream: %ux%u",
                        m_capture->getWidth(), m_capture->getHeight());
        }
        else
        {
            ImGui::Text("Idle.");
        }
    }
    ImGui::End();
}
