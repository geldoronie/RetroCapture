#pragma once

#include <string>

// Forward declarations
class UIManager;

/**
 * Aba Web Portal da janela de configuração
 */
class UIConfigurationWebPortal
{
public:
    UIConfigurationWebPortal(UIManager *uiManager);
    ~UIConfigurationWebPortal();

    void render();
    void setVisible(bool visible) { m_visible = visible; }
    bool isVisible() const        { return m_visible; }

private:
    bool m_visible = false;
    UIManager *m_uiManager = nullptr;

    void renderWebPortalEnable();
    void renderStartStopButton();
    void renderHTTPSSettings();
    void renderCustomization();
    void renderPortalURL();
};
