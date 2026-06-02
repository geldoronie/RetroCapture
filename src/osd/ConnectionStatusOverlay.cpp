#include "ConnectionStatusOverlay.h"
#include "QuickActionsOverlay.h"
#include "../ui/UIManager.h"
#include "../utils/TranslationManager.h"

#include <imgui.h>

ConnectionStatusOverlay::ConnectionStatusOverlay(UIManager *uiManager,
                                                 QuickActionsOverlay *quickActions)
    : m_uiManager(uiManager)
    , m_quickActions(quickActions)
{
}

ConnectionStatusOverlay::~ConnectionStatusOverlay() = default;

void ConnectionStatusOverlay::render()
{
    if (!m_uiManager) return;

    if (m_uiManager->getSourceType() != UIManager::SourceType::Remote)
    {
        // Not in client mode — nothing to surface. Reset the
        // transition tracking so a later mode switch starts fresh.
        m_lastDevice.clear();
        m_lastHadFrames      = false;
        m_connectedSince     = 0.0;
        m_disconnectingUntil = 0.0;
        return;
    }

    const double now      = ImGui::GetTime();
    const std::string dev = m_uiManager->getCurrentDevice();
    // 'Has frames' means decoding right NOW — capture dims stick at
    // the last seen value forever after a drop, so the dedicated
    // VideoCaptureRemote::isReceivingFrames mirror is the only
    // honest liveness signal.
    const bool hasFrames  = m_uiManager->getRemoteReceivingFrames();
    const bool offline    = m_uiManager->getRemoteHostLikelyOffline();
    // First-connect failure: this armed URL has never produced a frame
    // and the async connect has failed a few times. Distinct from a
    // mid-session reconnect (which keeps the calmer "Reconnecting…").
    const bool firstConnectFailing = m_uiManager->getRemoteInitialConnectFailing();

    // Disconnect tail — when the user clears the device, hold the
    // "Disconnecting…" label for a short window so the detached
    // teardown has visible feedback.
    if (!m_lastDevice.empty() && dev.empty())
    {
        m_disconnectingUntil = now + 1.5;
    }

    // First-frame-arrived edge → arm the "Connected" flash.
    if (!m_lastHadFrames && hasFrames && !dev.empty())
    {
        m_connectedSince = now;
    }
    if (!hasFrames)
    {
        m_connectedSince = 0.0;
    }

    std::string label;
    ImVec4      color    = ImVec4(0.95f, 0.7f, 0.3f, 1.0f); // orange default
    bool        spinning = false;

    if (now < m_disconnectingUntil)
    {
        label    = T("overlay.disconnecting");
        spinning = true;
    }
    else if (dev.empty())
    {
        // No URL armed and past the disconnect tail — nothing to say.
    }
    else if (offline)
    {
        label    = T("overlay.host_offline");
        spinning = true;
    }
    else if (!hasFrames && firstConnectFailing)
    {
        // Never connected and the retries are failing — tell the user
        // plainly rather than spinning "Connecting…" indefinitely. The
        // URL stays armed and keeps retrying in the background.
        label    = T("overlay.connect_failed");
        color    = ImVec4(0.95f, 0.45f, 0.40f, 1.0f); // red-ish
        spinning = true;
    }
    else if (!hasFrames)
    {
        label    = m_lastHadFrames ? T("overlay.reconnecting")
                                   : T("overlay.connecting");
        spinning = true;
    }
    else if (m_connectedSince > 0.0 && now - m_connectedSince < 3.0)
    {
        label    = T("overlay.connected");
        color    = ImVec4(0.40f, 0.80f, 0.40f, 1.0f);
        spinning = false;
    }

    // Update tracking before the early-return so transitions
    // detected this frame land in next frame's state.
    m_lastDevice    = dev;
    m_lastHadFrames = hasFrames;

    if (label.empty()) return;

    // Shared bottom-right corner with QuickActionsOverlay (#68). When
    // the widget is on screen, query its rendered height and push
    // ourselves up by that + a small gap. No magic numbers — the
    // overlay grows/shrinks if the widget's content changes.
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    float bottomReserved = 16.0f;
    if (m_quickActions && m_quickActions->isVisible())
    {
        const float widgetH = m_quickActions->renderedHeight();
        if (widgetH > 0.0f) bottomReserved += widgetH + 8.0f;
    }
    const ImVec2 anchor(vp->WorkPos.x + vp->WorkSize.x - 16.0f,
                        vp->WorkPos.y + vp->WorkSize.y - bottomReserved);
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration       | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings    | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav              | ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("##connOverlay", nullptr, flags))
    {
        if (spinning)
        {
            // 4-frame ASCII spinner at ~6 fps. ImGui doesn't ship a
            // spinner widget; a one-char rotation is enough to signal
            // "working" without pulling extra primitives.
            static const char *spin = "|/-\\";
            const int idx = static_cast<int>(now * 6.0) & 0x3;
            ImGui::TextColored(color, "%c %s", spin[idx], label.c_str());
        }
        else
        {
            ImGui::TextColored(color, "%s", label.c_str());
        }
    }
    ImGui::End();
}
