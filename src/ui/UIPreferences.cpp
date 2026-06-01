#include "UIPreferences.h"
#include "UIManager.h"
#include "UISectionHeader.h"
#include "../utils/TranslationManager.h"
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
    if (!ImGui::Begin(T("preferences.title").c_str(), &m_visible))
    {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("%s", T("preferences.intro").c_str());

    // Changing this calls TranslationManager::setLanguage, which
    // reloads the overlay bundle in-place; the next frame's T()
    // lookups already render in the new language.
    ui_section_header(T("preferences.language").c_str());
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
        const std::string newLang = langKeys[langIdx];
        m_uiManager->setLanguage(newLang);
        TranslationManager::instance().setLanguage(newLang);
        m_uiManager->saveConfig();
    }
    ImGui::TextDisabled("%s", T("preferences.language.hint").c_str());

    ui_section_header(T("preferences.window").c_str());
    bool startFullscreen = m_uiManager->getStartFullscreen();
    if (ImGui::Checkbox(T("preferences.start_fullscreen").c_str(), &startFullscreen))
    {
        m_uiManager->setStartFullscreen(startFullscreen);
        m_uiManager->saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", T("preferences.start_fullscreen.tip").c_str());
    }

    bool quickActionsAutoHide = m_uiManager->getQuickActionsAutoHide();
    if (ImGui::Checkbox(T("preferences.quickactions_autohide").c_str(), &quickActionsAutoHide))
    {
        m_uiManager->setQuickActionsAutoHide(quickActionsAutoHide);
        m_uiManager->saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", T("preferences.quickactions_autohide.tip").c_str());
    }

    // ── System tray / background operation (#86) ──────────────────
    ui_section_header(T("preferences.tray").c_str());

    bool trayEnabled = m_uiManager->getTrayEnabled();
    if (ImGui::Checkbox(T("preferences.tray.enabled").c_str(), &trayEnabled))
    {
        m_uiManager->setTrayEnabled(trayEnabled);
        m_uiManager->saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", T("preferences.tray.enabled.tip").c_str());
    }

    // The sub-options only matter when the tray is on.
    ImGui::BeginDisabled(!trayEnabled);

    bool minimizeOnClose = m_uiManager->getTrayMinimizeOnClose();
    if (ImGui::Checkbox(T("preferences.tray.minimize_on_close").c_str(), &minimizeOnClose))
    {
        m_uiManager->setTrayMinimizeOnClose(minimizeOnClose);
        m_uiManager->saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", T("preferences.tray.minimize_on_close.tip").c_str());
    }

    bool startMinimized = m_uiManager->getTrayStartMinimized();
    if (ImGui::Checkbox(T("preferences.tray.start_minimized").c_str(), &startMinimized))
    {
        m_uiManager->setTrayStartMinimized(startMinimized);
        m_uiManager->saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", T("preferences.tray.start_minimized.tip").c_str());
    }

    bool trayNotifications = m_uiManager->getTrayNotifications();
    if (ImGui::Checkbox(T("preferences.tray.notifications").c_str(), &trayNotifications))
    {
        m_uiManager->setTrayNotifications(trayNotifications);
        m_uiManager->saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("%s", T("preferences.tray.notifications.tip").c_str());
    }

    ImGui::EndDisabled();

    ImGui::End();
}
