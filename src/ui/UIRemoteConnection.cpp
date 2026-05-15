#include "UIRemoteConnection.h"
#include "UIManager.h"
#include "../streaming/DirectoryBrowser.h"
#include "../utils/PasswordHash.h"

#include <imgui.h>

#ifndef RETROCAPTURE_VERSION
#define RETROCAPTURE_VERSION "0.0.0-dev"
#endif

#include <algorithm>
#include <cctype>
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

void UIRemoteConnection::triggerConnect(const std::string &url)
{
    std::string clean = url;
    while (!clean.empty() && clean.back() == '/') clean.pop_back();
    if (clean.empty()) return;

    // Mirror into the URL input so the user sees what they just
    // committed to, plus arm the existing state machine. Pop the
    // window open so the connection feedback is visible — the user
    // may have arrived here from the Browse window.
    std::strncpy(m_urlBuffer, clean.c_str(), sizeof(m_urlBuffer) - 1);
    m_urlBuffer[sizeof(m_urlBuffer) - 1] = '\0';
    m_pendingUrl = clean;
    m_pending    = PendingAction::ConnectShowStatus;
    m_visible    = true;
}

void UIRemoteConnection::render()
{
    if (!m_visible || !m_uiManager) return;

    // Source-aware: getCurrentDevice() returns whatever the active capture
    // path needs as its "device" — for V4L2/DS that's a filesystem path
    // like /dev/video0, NOT a URL. Treat it as a URL only when the source
    // is already Remote; otherwise seed the buffer with the default.
    const bool sourceIsRemote = (m_uiManager->getSourceType() == UIManager::SourceType::Remote);
    if (!m_urlSeeded)
    {
        std::string current = sourceIsRemote ? m_uiManager->getCurrentDevice() : std::string();
        if (current.empty()) current = "http://localhost:8080";
        std::strncpy(m_urlBuffer, current.c_str(), sizeof(m_urlBuffer) - 1);
        m_urlBuffer[sizeof(m_urlBuffer) - 1] = '\0';
        m_urlSeeded = true;
    }
    else if (sourceIsRemote)
    {
        // While in Remote mode, keep the buffer mirroring the active
        // device path (so Disconnect-then-reopen shows the URL the user
        // was last on, and an external --remote-url switch is reflected).
        // Do NOT overwrite while typing — only sync when the user isn't
        // editing this field.
        if (!ImGui::IsItemActive())
        {
            std::string current = m_uiManager->getCurrentDevice();
            if (!current.empty() && current != std::string(m_urlBuffer))
            {
                std::strncpy(m_urlBuffer, current.c_str(), sizeof(m_urlBuffer) - 1);
                m_urlBuffer[sizeof(m_urlBuffer) - 1] = '\0';
            }
        }
    }

    const std::string currentDevice = sourceIsRemote ? m_uiManager->getCurrentDevice() : std::string();
    const bool connected = sourceIsRemote && !currentDevice.empty();

    ImGui::SetNextWindowSize(ImVec2(520, 320), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Connect to Remote", &m_visible))
    {
        ImGui::TextWrapped(
            "Consume a remote RetroCapture stream. The client decodes the "
            "host's /raw feed and mirrors its shader pipeline via /meta.");
        ImGui::Spacing();
        ImGui::TextDisabled("Tip: Remote → Browse public directory… to pick from the public list.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        renderManualTab(sourceIsRemote, currentDevice, connected);

        ImGui::Spacing();
        ImGui::Separator();
        renderStatusFooter(connected);
    }
    ImGui::End();

    // Always advance the state machine even if the window is now
    // covered — connect/disconnect MUST progress once initiated.
    advanceStateMachine();
}

// ─────────────────────────────────────────────────────────────────────
// Manual URL tab — the original flow.
// ─────────────────────────────────────────────────────────────────────
void UIRemoteConnection::renderManualTab(bool /*sourceIsRemote*/,
                                         const std::string &currentDevice,
                                         bool connected)
{
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

    // Two-frame state machine: button click sets *ShowStatus, the
    // next render of this state shows the spinning label and arms
    // *Execute, the frame after runs the blocking call. The user
    // gets one painted 'Connecting...' / 'Disconnecting...' before
    // the freeze actually starts.
    const bool inProgress = (m_pending != PendingAction::None);
    if (!connected)
    {
        ImGui::BeginDisabled(inProgress);
        if (ImGui::Button("Connect", ImVec2(120, 0)))
        {
            std::string url(m_urlBuffer);
            // Strip a trailing slash so the appended /raw and /meta land
            // cleanly down in VideoCaptureRemote / RemoteMetaSync.
            while (!url.empty() && url.back() == '/') url.pop_back();
            if (!url.empty())
            {
                m_pendingUrl = url;
                m_pending    = PendingAction::ConnectShowStatus;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (m_pending == PendingAction::ConnectShowStatus ||
            m_pending == PendingAction::ConnectExecute)
        {
            ImGui::TextDisabled("connecting...");
        }
        else
        {
            ImGui::TextDisabled("not connected");
        }
    }
    else
    {
        ImGui::BeginDisabled(inProgress);
        if (ImGui::Button("Disconnect", ImVec2(120, 0)))
        {
            m_pending = PendingAction::DisconnectShowStatus;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (m_pending == PendingAction::DisconnectShowStatus ||
            m_pending == PendingAction::DisconnectExecute)
        {
            ImGui::TextDisabled("disconnecting...");
        }
        else
        {
            ImGui::TextDisabled("connected to %s", currentDevice.c_str());
        }
    }
}

// ─────────────────────────────────────────────────────────────────────
// Footer
// ─────────────────────────────────────────────────────────────────────
void UIRemoteConnection::renderStatusFooter(bool connected)
{
    ImGui::TextDisabled("Status");
    if (connected)
    {
        const uint32_t w = m_uiManager->getCaptureWidth();
        const uint32_t h = m_uiManager->getCaptureHeight();
        if (w > 0 && h > 0) ImGui::Text("Stream: %ux%u", w, h);
        else                ImGui::Text("Connecting...");
    }
    else if (m_pending == PendingAction::ConnectShowStatus ||
             m_pending == PendingAction::ConnectExecute)
    {
        ImGui::Text("Connecting…");
    }
    else
    {
        ImGui::Text("Idle.");
    }
}

void UIRemoteConnection::advanceStateMachine()
{
    switch (m_pending)
    {
        case PendingAction::ConnectShowStatus:
            m_pending = PendingAction::ConnectExecute;
            break;
        case PendingAction::ConnectExecute:
            if (m_uiManager->getSourceType() != UIManager::SourceType::Remote)
            {
                m_uiManager->triggerSourceTypeChange(UIManager::SourceType::Remote);
            }
            m_uiManager->setCurrentDevice(m_pendingUrl);
            m_pendingUrl.clear();
            m_pending = PendingAction::None;
            break;
        case PendingAction::DisconnectShowStatus:
            m_pending = PendingAction::DisconnectExecute;
            break;
        case PendingAction::DisconnectExecute:
            m_uiManager->setCurrentDevice("");
            m_pending = PendingAction::None;
            break;
        case PendingAction::None:
            break;
    }
}
