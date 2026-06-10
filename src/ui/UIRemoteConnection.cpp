#include "UIRemoteConnection.h"
#include "UIManager.h"
#include "UISectionHeader.h"
#include "../capture/IVideoCapture.h"
#include "../streaming/DirectoryBrowser.h"
#include "../utils/PasswordHash.h"
#include "../utils/TranslationManager.h"

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
    // committed to, plus arm the existing state machine. We
    // intentionally do NOT flip m_visible here — when the user
    // clicked Connect on a Browse-window row they're already in a
    // window that gives them context, and popping the
    // Connect-to-Remote window on top is noise. Status surfaces via
    // the directory status text + the Info tab's Connection block
    // anyway. The user can still open this window manually from the
    // Remote menu if they want the focused view.
    std::strncpy(m_urlBuffer, clean.c_str(), sizeof(m_urlBuffer) - 1);
    m_urlBuffer[sizeof(m_urlBuffer) - 1] = '\0';
    m_pendingUrl = clean;
    m_pending    = PendingAction::ConnectShowStatus;
}

void UIRemoteConnection::render()
{
    if (!m_uiManager) return;

    // The state machine must advance every frame regardless of
    // whether this window is visible — when a Browse-window
    // 'Connect' arms a ConnectShowStatus / ConnectExecute step,
    // it needs to progress even with the Connect-to-Remote window
    // hidden. Drive the state first, then render the window only
    // if the user actually opened it.
    advanceStateMachine();
    if (!m_visible) return;

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
    if (ImGui::Begin(T("remote.title").c_str(), &m_visible))
    {
        ui_section_header(T("remote.title").c_str(), T("remote.intro").c_str());
        ImGui::TextDisabled("%s", T("remote.tip").c_str());

        renderManualTab(sourceIsRemote, currentDevice, connected);

        ui_section_header("Status");
        renderStatusFooter(connected);
    }
    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────
// Manual URL tab — the original flow.
// ─────────────────────────────────────────────────────────────────────
void UIRemoteConnection::renderManualTab(bool /*sourceIsRemote*/,
                                         const std::string &currentDevice,
                                         bool connected)
{
    ui_section_header(T("remote.base_url").c_str());
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
    ui_section_header(T("remote.display_interp").c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##remoteInterp", &currentModeIndex, modeLabels, 3))
    {
        m_uiManager->triggerRemoteInterpolationChange(modes[currentModeIndex]);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", T("remote.interp.tip").c_str());
    }

    ImGui::Spacing();

    // #77 Client-side audio volume + mute. Only meaningful in Remote
    // source mode, which is exactly where this tab lives. The slider
    // shows 0–100%; mute is a separate latch so unmuting restores the
    // last level. Both push through the UIManager triggers, which
    // persist and forward the effective gain to the active capture.
    ui_section_header(T("remote.audio_volume").c_str());
    bool  muted     = m_uiManager->getRemoteAudioMuted();
    float volumePct = m_uiManager->getRemoteAudioVolume() * 100.0f;

    if (ImGui::Checkbox(T("remote.audio_mute").c_str(), &muted))
    {
        m_uiManager->triggerRemoteAudioMuteChange(muted);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::BeginDisabled(muted);
    if (ImGui::SliderFloat("##remoteAudioVol", &volumePct, 0.0f, 100.0f, "%.0f%%"))
    {
        m_uiManager->triggerRemoteAudioVolumeChange(volumePct / 100.0f);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", T("remote.audio_vol_tip").c_str());
    }

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
        if (ImGui::Button(T("remote.connect").c_str(), ImVec2(120, 0)))
        {
            std::string url(m_urlBuffer);
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
            ImGui::TextDisabled("%s", T("remote.connecting").c_str());
        }
        else
        {
            ImGui::TextDisabled("%s", T("remote.not_connected").c_str());
        }
    }
    else
    {
        ImGui::BeginDisabled(inProgress);
        if (ImGui::Button(T("remote.disconnect").c_str(), ImVec2(120, 0)))
        {
            m_pending = PendingAction::DisconnectShowStatus;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (m_pending == PendingAction::DisconnectShowStatus ||
            m_pending == PendingAction::DisconnectExecute)
        {
            ImGui::TextDisabled("%s", T("remote.disconnecting").c_str());
        }
        else
        {
            ImGui::TextDisabled("%s %s", T("remote.connected_to").c_str(), currentDevice.c_str());
        }
    }
}

// ─────────────────────────────────────────────────────────────────────
// Footer
// ─────────────────────────────────────────────────────────────────────
void UIRemoteConnection::renderStatusFooter(bool connected)
{
    ImGui::TextDisabled("%s", T("remote.status").c_str());
    if (connected)
    {
        const uint32_t w = m_uiManager->getCaptureWidth();
        const uint32_t h = m_uiManager->getCaptureHeight();
        if (w > 0 && h > 0) ImGui::Text("%s %ux%u", T("remote.stream_dims").c_str(), w, h);
        else                ImGui::Text("%s", T("remote.connecting").c_str());
    }
    else if (m_pending == PendingAction::ConnectShowStatus ||
             m_pending == PendingAction::ConnectExecute)
    {
        ImGui::Text("%s", T("remote.connecting").c_str());
    }
    else
    {
        ImGui::Text("%s", T("remote.idle").c_str());
    }

    // #58 — surface the prolonged-reconnect-failure hint. Visible on
    // any state because once the host is suspected offline the user
    // wants the message regardless of whether we're mid-handshake or
    // already given up on this attempt. Cleared automatically by
    // VideoCaptureRemote on the next successful reconnect.
    //
    // The flag is mirrored onto UIManager every frame by
    // Application::syncDirectoryClient — m_uiManager->getCapture()
    // returns null in Remote mode (Application passes nullptr to
    // setCaptureControls to hide the V4L2/DS hardware-controls UI),
    // so we'd never see the flag if we went through that pointer.
    if (m_uiManager && m_uiManager->getRemoteHostLikelyOffline())
    {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1.0f),
                           "%s", T("remote.host_offline").c_str());
        ImGui::TextWrapped("%s", T("remote.host_offline.hint").c_str());
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
