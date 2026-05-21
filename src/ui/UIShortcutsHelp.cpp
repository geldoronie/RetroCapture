#include "UIShortcutsHelp.h"
#include "UIManager.h"
#include "../utils/TranslationManager.h"

#include <imgui.h>

UIShortcutsHelp::UIShortcutsHelp(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UIShortcutsHelp::~UIShortcutsHelp() = default;

void UIShortcutsHelp::render()
{
    if (!m_visible || !m_uiManager) return;

    // Pinned to top-right every frame. ImGuiCond_Always + pivot (1,0)
    // anchors the top-right corner of the window to the viewport's
    // top-right padded by 16 px.
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const ImVec2 anchor(vp->WorkPos.x + vp->WorkSize.x - 16.0f,
                        vp->WorkPos.y + 16.0f);
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                   ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_AlwaysAutoResize;

    if (!ImGui::Begin("##shortcutsHelp", nullptr, flags))
    {
        ImGui::End();
        return;
    }

    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "%s",
                       T("shortcuts.title").c_str());
    ImGui::Separator();

    // Two-column layout: shortcut key on the left, description on
    // the right. Using ImGui::Text + SameLine instead of a Table
    // because the table widget pulls in scroll/sort scaffolding we
    // don't need for a five-item card.
    auto row = [](const char *key, const char *desc) {
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.4f, 1.0f), "%s", key);
        ImGui::SameLine(80.0f);
        ImGui::TextDisabled("%s", desc);
    };

    row("F11", T("shortcuts.f11").c_str());
    row("F12", T("shortcuts.f12").c_str());

    ImGui::Spacing();
    ImGui::TextDisabled("%s", T("shortcuts.dismiss_hint").c_str());

    ImGui::End();
}
