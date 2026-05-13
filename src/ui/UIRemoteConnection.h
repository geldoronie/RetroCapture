#pragma once

#include <string>

class UIManager;
class IVideoCapture;

/**
 * UIRemoteConnection — dedicated window for the Remote viewer mode.
 *
 * Surfaces:
 *  - Remote base URL input
 *  - Display interpolation dropdown (linear / nearest / off)
 *  - Connect / Disconnect button
 *  - Connection status / stream dims
 *
 * Lives outside the Source tab on purpose: connecting to a remote
 * RetroCapture instance is conceptually a different operating mode
 * ("client viewer") rather than a capture-source choice, so it sits
 * under its own top-level menu.
 */
class UIRemoteConnection
{
public:
    UIRemoteConnection(UIManager *uiManager);
    ~UIRemoteConnection();

    void render();
    void setVisible(bool visible);
    bool isVisible() const { return m_visible; }

    // Capture pointer is consulted to decide whether to show
    // Connect or Disconnect, and to read the live status / dims.
    void setCapture(IVideoCapture *capture) { m_capture = capture; }

private:
    UIManager *m_uiManager = nullptr;
    IVideoCapture *m_capture = nullptr;
    bool m_visible = false;

    // ImGui InputText buffer, seeded from the saved device path on
    // first render so the user's previous URL persists across sessions.
    char m_urlBuffer[256] = {};
    bool m_urlSeeded = false;
};
