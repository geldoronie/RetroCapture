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

private:
    UIManager *m_uiManager = nullptr;

    void renderBrightnessContrast();
    void renderAspectRatio();
    void renderFullscreen();
    void renderMonitorSelection();
    void renderOutputResolution();
};
