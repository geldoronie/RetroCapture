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

private:
    UIManager *m_uiManager = nullptr;

    void renderCaptureInfo();
    void renderStreamingInfo();
    void renderSystemInfo();
};
