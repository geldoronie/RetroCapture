#include "UIRemoteConnection.h"
#include "UIManager.h"
#include "../streaming/DirectoryBrowser.h"

#include <imgui.h>

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

    ImGui::SetNextWindowSize(ImVec2(680, 460), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Connect to Remote", &m_visible))
    {
        ImGui::TextWrapped(
            "Consume a remote RetroCapture stream. The client decodes the "
            "host's /raw feed and mirrors its shader pipeline via /meta.");
        ImGui::Spacing();

        if (ImGui::BeginTabBar("##remoteTabs"))
        {
            if (ImGui::BeginTabItem("Manual URL"))
            {
                renderManualTab(sourceIsRemote, currentDevice, connected);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Browse directory"))
            {
                renderBrowseTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Spacing();
        ImGui::Separator();
        renderStatusFooter(connected);
    }
    ImGui::End();

    // Always advance the state machine even if the window is now
    // covered behind a tab change — connect/disconnect MUST progress
    // once initiated.
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
// Browse directory tab (#49 Phase 4)
// ─────────────────────────────────────────────────────────────────────

namespace
{
    // Case-insensitive substring match — same idea as the server-side
    // filter, but applied locally so the search field is instantly
    // responsive without round-tripping to the directory.
    bool containsCI(const std::string &haystack, const std::string &needle)
    {
        if (needle.empty()) return true;
        auto h = haystack;
        auto n = needle;
        for (auto &c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (auto &c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return h.find(n) != std::string::npos;
    }

    const char *endpointModeIcon(const std::string &mode)
    {
        if (mode == "tunnel-cloudflare") return "tunnel";
        if (mode == "custom")            return "custom";
        return "direct";
    }
}

void UIRemoteConnection::renderBrowseTab()
{
    ImGui::Spacing();
    if (!m_browser)
    {
        ImGui::TextDisabled("Directory browser unavailable.");
        return;
    }

    // Directory URL field — same setting the publish side uses, kept
    // in UIManager so it survives across runs.
    {
        char urlBuf[256];
        std::snprintf(urlBuf, sizeof(urlBuf), "%s", m_uiManager->getDirectoryUrl().c_str());
        ImGui::Text("Directory");
        ImGui::SetNextItemWidth(-120);
        if (ImGui::InputText("##dirUrl", urlBuf, sizeof(urlBuf)))
        {
            m_uiManager->setDirectoryUrl(urlBuf);
            m_uiManager->saveConfig();
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh", ImVec2(112, 0)))
        {
            m_browser->refreshNow();
        }
    }

    // Sort + search.
    {
        const char *sortLabels[] = { "Most viewers", "Recently active", "Name (A-Z)" };
        ImGui::SetNextItemWidth(180);
        ImGui::Combo("Sort", &m_browseSortIndex, sortLabels, 3);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##browseSearch", "Search name / nickname", m_browseSearch, sizeof(m_browseSearch));
    }

    auto snap = m_browser->getSnapshot();

    // Local sort: snapshot order from the server is by client_count
    // desc; we re-sort here when the user picks a different key.
    auto entries = snap.entries;
    switch (m_browseSortIndex)
    {
        case 1: // recent — use expiresAt descending as a proxy for last heartbeat
            std::sort(entries.begin(), entries.end(),
                      [](const DirectoryBrowser::Entry &a, const DirectoryBrowser::Entry &b) {
                          return a.expiresAt > b.expiresAt;
                      });
            break;
        case 2: // name
            std::sort(entries.begin(), entries.end(),
                      [](const DirectoryBrowser::Entry &a, const DirectoryBrowser::Entry &b) {
                          auto la = a.name, lb = b.name;
                          for (auto &c : la) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                          for (auto &c : lb) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                          return la < lb;
                      });
            break;
        default: // 0 = clients (already the server order)
            break;
    }

    // Search filter.
    const std::string filter = m_browseSearch;
    if (!filter.empty())
    {
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&filter](const DirectoryBrowser::Entry &e) {
                                         return !containsCI(e.name, filter) &&
                                                !containsCI(e.hostNickname, filter);
                                     }),
                      entries.end());
    }

    ImGui::Spacing();
    if (!snap.lastError.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                           "Last fetch error: %s", snap.lastError.c_str());
    }

    // Table.
    const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg
                                     | ImGuiTableFlags_BordersInnerH
                                     | ImGuiTableFlags_ScrollY
                                     | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##dirTable", 7, tableFlags, ImVec2(0, 260)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Host",     ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Shader",   ImGuiTableColumnFlags_WidthStretch, 2.5f);
        ImGui::TableSetupColumn("Res×FPS",  ImGuiTableColumnFlags_WidthFixed,   95.0f);
        ImGui::TableSetupColumn("Codec",    ImGuiTableColumnFlags_WidthFixed,   45.0f);
        ImGui::TableSetupColumn("Clients",  ImGuiTableColumnFlags_WidthFixed,   60.0f);
        ImGui::TableSetupColumn("Mode",     ImGuiTableColumnFlags_WidthFixed,   80.0f);
        ImGui::TableHeadersRow();

        if (entries.empty())
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", snap.totalCount == 0
                                          ? "No public streams right now."
                                          : "No matches for the current filter.");
        }
        for (const auto &e : entries)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // Selectable spans the row so the click target is generous.
            std::string label = e.name;
            if (e.passwordRequired) label += " [locked]";
            const bool clicked = ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);
            ImGui::TableNextColumn();
            ImGui::Text("%s", e.hostNickname.empty() ? "—" : e.hostNickname.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s", e.shader.empty() ? "—" : e.shader.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%ux%u@%u", e.resolutionW, e.resolutionH, e.fps);
            ImGui::TableNextColumn();
            ImGui::Text("%s", e.codec.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d", e.clientCount);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", endpointModeIcon(e.endpointMode));

            if (clicked)
            {
                // Stash the URL → next frame we mirror into the Manual
                // tab's buffer and arm the connect state machine. We
                // don't run the connect inline because we're inside a
                // tab item that switches away when the user expects
                // the action to land on the Manual tab.
                m_browseSelectedUrl = e.endpoint;
            }
        }
        ImGui::EndTable();
    }

    // Honour a click from the previous frame: mirror the URL into the
    // Manual tab buffer and arm Connect. We intentionally do NOT
    // auto-switch tabs — the user just clicked, they know what they
    // want; flipping their tab under them is annoying. The Status
    // footer below the tabs will show the connecting/connected
    // feedback regardless of which tab is active.
    if (!m_browseSelectedUrl.empty())
    {
        std::string url = m_browseSelectedUrl;
        m_browseSelectedUrl.clear();
        while (!url.empty() && url.back() == '/') url.pop_back();
        if (!url.empty())
        {
            std::strncpy(m_urlBuffer, url.c_str(), sizeof(m_urlBuffer) - 1);
            m_urlBuffer[sizeof(m_urlBuffer) - 1] = '\0';
            m_pendingUrl = url;
            m_pending    = PendingAction::ConnectShowStatus;
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Showing %zu of %d total stream(s). Auto-refresh every 30 s.",
                        entries.size(), snap.totalCount);
}

// ─────────────────────────────────────────────────────────────────────
// Footer shared by both tabs.
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
