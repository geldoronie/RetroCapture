#pragma once

// Forward declarations
class UIManager;

/**
 * Preferences window — application-level behaviour settings that
 * aren't tied to a specific capture / streaming / recording
 * pipeline. Accessible from File → Preferences... on the main
 * menu bar.
 *
 * v1 scope (Fase A of the i18n + window-split work):
 *   - Language dropdown (placeholder until Fase B wires
 *     TranslationManager + en/pt JSON resources)
 *   - "Start in fullscreen" checkbox (previously a one-off CLI
 *     flag; now persists in config.json)
 *
 * Things that explicitly do NOT live here:
 *   - Anything pipeline-specific (codec / bitrate / shader /
 *     source — those have their own dedicated windows under the
 *     Configurations menu).
 */
class UIPreferences
{
public:
    explicit UIPreferences(UIManager *uiManager);
    ~UIPreferences();

    void render();
    void setVisible(bool visible) { m_visible = visible; }
    bool isVisible() const        { return m_visible; }

private:
    bool        m_visible    = false;
    UIManager  *m_uiManager  = nullptr;
};
