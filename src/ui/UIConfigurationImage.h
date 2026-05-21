#pragma once

// Forward declarations
class UIManager;

/**
 * Aba Image da janela de configuração
 */
class UIConfigurationImage
{
public:
    UIConfigurationImage(UIManager *uiManager);
    ~UIConfigurationImage();

    void render();
    void setVisible(bool visible) { m_visible = visible; }
    bool isVisible() const        { return m_visible; }

private:
    bool m_visible = false;
    UIManager *m_uiManager = nullptr;

    void renderBrightnessContrast();
    void renderAspectRatio();
    void renderFullscreen();
    void renderMonitorSelection();
    void renderOutputResolution();
};
