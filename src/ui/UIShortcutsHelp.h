#pragma once

class UIManager;

/**
 * Keyboard-shortcuts orientation widget (#68 follow-up).
 *
 * Lives in the UI layer (`src/ui/`) — visibility tracks F12 like
 * every other interactive window, NOT the OSD layer. The intent is
 * a discoverability nudge for new users, not a permanent HUD:
 * when the user presses F12 to clear the screen, the shortcuts
 * hint goes away with the rest of the UI.
 *
 * Pinned to the top-right corner of the viewport every frame so it
 * doesn't compete with the bottom-right OSD overlays
 * (QuickActionsOverlay + ConnectionStatusOverlay). No drag, no
 * resize, no decoration — close it via View → Shortcuts when
 * you've memorised the list.
 *
 * Visibility persists across runs via UIManager + config.json so the
 * user only has to dismiss it once.
 */
class UIShortcutsHelp
{
public:
    UIShortcutsHelp(UIManager *uiManager);
    ~UIShortcutsHelp();

    void render();
    void setVisible(bool visible) { m_visible = visible; }
    bool isVisible() const        { return m_visible; }

    // Height on its last rendered frame, so the FPS overlay (#226) can
    // stack below it when both share the top-right corner.
    float renderedHeight() const  { return m_lastRenderedHeight; }

private:
    UIManager *m_uiManager = nullptr;
    bool       m_visible = true;
    float      m_lastRenderedHeight = 0.0f;
};
