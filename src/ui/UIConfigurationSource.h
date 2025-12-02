#pragma once

#include <string>
#include <cstdint>
#include <vector>

// Forward declarations
class UIManager;
class VideoCapture;

/**
 * Aba Source da janela de configuração
 */
class UIConfigurationSource
{
public:
    UIConfigurationSource(UIManager *uiManager);
    ~UIConfigurationSource();

    void render();

private:
    UIManager *m_uiManager = nullptr;
    VideoCapture *m_capture = nullptr;

    void renderSourceTypeSelection();
    void renderV4L2Controls();
    void renderV4L2DeviceSelection();
    void renderCaptureSettings();
    void renderQuickResolutions();
    void renderQuickFPS();
};
