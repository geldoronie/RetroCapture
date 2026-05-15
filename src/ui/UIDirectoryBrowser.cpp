#include "UIDirectoryBrowser.h"
#include "UIManager.h"
#include "UIRemoteConnection.h"
#include "../streaming/DirectoryBrowser.h"
#include "../utils/PasswordHash.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#ifndef RETROCAPTURE_VERSION
#define RETROCAPTURE_VERSION "0.0.0-dev"
#endif

namespace
{
    bool containsCI(const std::string &haystack, const std::string &needle)
    {
        if (needle.empty()) return true;
        std::string h = haystack;
        std::string n = needle;
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

    // Draws a small padlock at the current cursor using ImDrawList
    // primitives. The default ImGui font ships ASCII + Latin-1 only, so
    // U+1F512 renders as the missing-glyph fallback ('?') — drawing
    // primitives sidestep the font question entirely.
    void drawPadlockIcon()
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float scale = ImGui::GetFontSize() / 13.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        const float bodyW   = 10.0f * scale;
        const float bodyH   =  7.0f * scale;
        const float radius  =  3.0f * scale;
        const float thick   =  1.5f * scale;
        const ImU32 color   = ImGui::GetColorU32(ImGuiCol_Text);

        // Vertical centering inside one text line.
        const float lineH = ImGui::GetTextLineHeight();
        const float iconH = bodyH + radius + thick * 0.5f;
        const float yPad  = (lineH - iconH) * 0.5f;
        const float x0    = origin.x;
        const float y0    = origin.y + (yPad > 0.0f ? yPad : 0.0f);

        const float PI = 3.14159265358979323846f;
        const ImVec2 shackleCenter(x0 + bodyW * 0.5f, y0 + radius);
        dl->PathArcTo(shackleCenter, radius, PI, 2.0f * PI, 16);
        dl->PathStroke(color, ImDrawFlags_None, thick);

        const ImVec2 bodyTL(x0,             y0 + radius);
        const ImVec2 bodyBR(x0 + bodyW,     y0 + radius + bodyH);
        dl->AddRectFilled(bodyTL, bodyBR, color, 1.0f * scale);

        // Reserve layout space so the cell behaves like a normal text
        // row (hover hit-testing, table sizing, etc.).
        ImGui::Dummy(ImVec2(bodyW, lineH));
    }
}

UIDirectoryBrowser::UIDirectoryBrowser(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UIDirectoryBrowser::~UIDirectoryBrowser() = default;

void UIDirectoryBrowser::render()
{
    if (!m_visible || !m_uiManager) return;

    ImGui::SetNextWindowSize(ImVec2(720, 480), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Browse public directory", &m_visible))
    {
        renderTable();
    }
    ImGui::End();

    // The password modal is opened from inside renderTable but
    // BeginPopupModal must run unconditionally each frame the popup
    // is on screen; rendering it here keeps it visible even if the
    // main window scrolls away.
    renderPasswordModal();
}

void UIDirectoryBrowser::renderTable()
{
    if (!m_browser)
    {
        ImGui::TextDisabled("Directory browser unavailable.");
        return;
    }

    ImGui::TextWrapped(
        "Live list of streams published to the public directory. "
        "Click a row to connect. Auto-refresh every 30 s.");
    ImGui::Spacing();

    // Directory URL — shared with the publish side via UIManager so
    // changing one updates the other.
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
        ImGui::Combo("Sort", &m_sortIndex, sortLabels, 3);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##browseSearch", "Search name / nickname",
                                 m_search, sizeof(m_search));
    }

    auto snap = m_browser->getSnapshot();

    // Local sort.
    auto entries = snap.entries;
    switch (m_sortIndex)
    {
        case 1:
            std::sort(entries.begin(), entries.end(),
                      [](const DirectoryBrowser::Entry &a, const DirectoryBrowser::Entry &b) {
                          return a.expiresAt > b.expiresAt;
                      });
            break;
        case 2:
            std::sort(entries.begin(), entries.end(),
                      [](const DirectoryBrowser::Entry &a, const DirectoryBrowser::Entry &b) {
                          auto la = a.name, lb = b.name;
                          for (auto &c : la) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                          for (auto &c : lb) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                          return la < lb;
                      });
            break;
        default:
            break;
    }

    // Filter.
    const std::string filter = m_search;
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

    const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg
                                     | ImGuiTableFlags_BordersInnerH
                                     | ImGuiTableFlags_ScrollY
                                     | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##dirTable", 8, tableFlags, ImVec2(0, -ImGui::GetFrameHeightWithSpacing())))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch, 3.0f);
        // Tiny status column for the password padlock. Header is short
        // text since the default ImGui font can't render U+1F512; the
        // padlock glyph in each cell is drawn via ImDrawList primitives.
        ImGui::TableSetupColumn("Pwd", ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableSetupColumn("Host",     ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Shader",   ImGuiTableColumnFlags_WidthStretch, 2.5f);
        ImGui::TableSetupColumn("Res\xc3\x97""FPS",  ImGuiTableColumnFlags_WidthFixed, 95.0f);
        ImGui::TableSetupColumn("Codec",    ImGuiTableColumnFlags_WidthFixed, 45.0f);
        ImGui::TableSetupColumn("Clients (\xe2\x89\x88)", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Mode",     ImGuiTableColumnFlags_WidthFixed, 80.0f);
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
            ImGui::PushID(e.streamId.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            const bool versionMismatch = !e.version.empty() &&
                                         e.version != std::string(RETROCAPTURE_VERSION);

            std::string label;
            if (versionMismatch) label += "\xe2\x9a\xa0 ";
            label += e.name;
            const bool clicked = ImGui::Selectable(label.c_str(), false,
                                                   ImGuiSelectableFlags_SpanAllColumns);
            if (versionMismatch && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Host version: %s\nThis client: %s\n"
                    "Wire protocol may differ — connection may fail or behave oddly.",
                    e.version.c_str(), RETROCAPTURE_VERSION);
            }
            ImGui::TableNextColumn();
            if (e.passwordRequired)
            {
                drawPadlockIcon();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Password required");
            }
            ImGui::TableNextColumn();
            ImGui::Text("%s", e.hostNickname.empty() ? "\xe2\x80\x94" : e.hostNickname.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s", e.shader.empty() ? "\xe2\x80\x94" : e.shader.c_str());
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
                if (e.passwordRequired)
                {
                    m_pendingProtectedUrl = e.endpoint;
                    m_passwordBuffer[0]   = '\0';
                    m_showPasswordModal   = true;
                }
                else if (m_remoteWindow)
                {
                    if (m_uiManager) m_uiManager->setRemoteAuthToken("");
                    m_remoteWindow->triggerConnect(e.endpoint);
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::TextDisabled("Showing %zu of %d total stream(s).", entries.size(), snap.totalCount);
}

void UIDirectoryBrowser::renderPasswordModal()
{
    if (m_showPasswordModal)
    {
        ImGui::OpenPopup("Stream password");
        m_showPasswordModal = false;
    }
    if (ImGui::BeginPopupModal("Stream password", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("This stream is password-protected. Enter the password "
                           "the host configured to connect.");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(280);
        bool submitted = ImGui::InputText("##pw", m_passwordBuffer, sizeof(m_passwordBuffer),
                                          ImGuiInputTextFlags_Password |
                                          ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        const bool accept = ImGui::Button("Connect", ImVec2(120, 0)) || submitted;
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            m_pendingProtectedUrl.clear();
            std::memset(m_passwordBuffer, 0, sizeof(m_passwordBuffer));
            ImGui::CloseCurrentPopup();
        }
        if (accept && !m_pendingProtectedUrl.empty())
        {
            if (m_uiManager)
            {
                m_uiManager->setRemoteAuthToken(
                    PasswordHash::sha256Hex(std::string(m_passwordBuffer)));
            }
            if (m_remoteWindow) m_remoteWindow->triggerConnect(m_pendingProtectedUrl);
            m_pendingProtectedUrl.clear();
            std::memset(m_passwordBuffer, 0, sizeof(m_passwordBuffer));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
