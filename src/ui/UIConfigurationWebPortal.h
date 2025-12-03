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

private:
    UIManager *m_uiManager = nullptr;

    void renderWebPortalEnable();
    void renderStartStopButton();
    void renderHTTPSSettings();
    void renderCustomization();
    void renderPortalURL();
};
