#include "UIPreferences.h"
#include "UIManager.h"
#include <imgui.h>

UIPreferences::UIPreferences(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UIPreferences::~UIPreferences() = default;

void UIPreferences::render()
{
    if (!m_visible || !m_uiManager) return;

    ImGui::SetNextWindowSize(ImVec2(440, 280), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Preferences", &m_visible))
    {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled(
        "Application-level behaviour. Pipeline settings (codec,\n"
        "bitrate, shader, source) live under the Configurations menu.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Language ──────────────────────────────────────────────────
    //
    // Placeholder for Fase B (#45). The actual TranslationManager +
    // resource bundles aren't here yet; this dropdown just persists
    // the choice into config so a later patch can read it on startup
    // and load the matching `en.json` / `pt.json`. Until then the
    // selection has no visible effect.
    ImGui::Text("Language");
    const char *langLabels[] = { "English", "Português (Brasil)" };
    const char *langKeys[]   = { "en",      "pt" };
    const std::string currentLang = m_uiManager->getLanguage();
    int langIdx = 0;
    for (int i = 0; i < 2; ++i)
    {
        if (currentLang == langKeys[i]) { langIdx = i; break; }
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##lang", &langIdx, langLabels, 2))
    {
        m_uiManager->setLanguage(langKeys[langIdx]);
        m_uiManager->saveConfig();
    }
    ImGui::TextDisabled("Translation infrastructure lands in a follow-up\n"
                        "commit (#45). The selection persists today but\n"
                        "the visible UI stays in English for now.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Window behaviour ──────────────────────────────────────────
    ImGui::Text("Window");
    bool startFullscreen = m_uiManager->getStartFullscreen();
    if (ImGui::Checkbox("Start in fullscreen", &startFullscreen))
    {
        m_uiManager->setStartFullscreen(startFullscreen);
        m_uiManager->saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("When on, RetroCapture opens fullscreen on launch.\n"
                          "Equivalent to passing --fullscreen on the CLI.\n"
                          "F11 toggles fullscreen at runtime regardless.");
    }

    ImGui::End();
}
