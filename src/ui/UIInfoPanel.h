#pragma once

#include <string>
#include <cstdint>

// Forward declarations
class UIManager;

/**
 * Aba Info (informações do sistema)
 */
class UIInfoPanel
{
public:
    UIInfoPanel(UIManager *uiManager);
    ~UIInfoPanel();

    void render();
    void setVisible(bool visible) { m_visible = visible; }
    bool isVisible() const        { return m_visible; }

private:
    bool m_visible = false;
    UIManager *m_uiManager = nullptr;

    void renderCaptureInfo();
    void renderStreamingInfo();
    void renderRemoteInfo();   // shown in place of capture+streaming when in client mode
    void renderSystemInfo();
};
