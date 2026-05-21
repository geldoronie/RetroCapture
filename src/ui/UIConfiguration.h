#pragma once

#include <string>
#include <cstdint>
#include <functional>
#include <vector>

// Forward declarations
class UIManager;
class IVideoCapture;
class ShaderEngine;

/**
 * Janela principal de configuração com abas
 */
class UIConfiguration
{
public:
    UIConfiguration(UIManager *uiManager);
    ~UIConfiguration();

    void render();
    void setVisible(bool visible);
    bool isVisible() const { return m_visible; }
    void setJustOpened(bool justOpened) { m_justOpened = justOpened; }

private:
    UIManager *m_uiManager = nullptr;
    bool m_visible = false;
    bool m_justOpened = false;
};
