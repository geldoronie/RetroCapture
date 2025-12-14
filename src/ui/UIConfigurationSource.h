#pragma once

#include <string>
#include <cstdint>
#include <vector>

// Forward declarations
class UIManager;
class IVideoCapture;

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
    IVideoCapture *m_capture = nullptr;

    void renderSourceTypeSelection();
    void renderV4L2Controls();
    void renderV4L2DeviceSelection();
#ifdef _WIN32
    void renderDSControls();
    void renderDSDeviceSelection();
#endif
    void renderCaptureSettings();
    void renderQuickResolutions();
    void renderQuickFPS();
};
