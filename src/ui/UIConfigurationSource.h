#pragma once

#include <string>
#include <cstdint>
#include <vector>

#ifdef __APPLE__
// Pulled in (not forward-declared) because m_avfDevices and
// m_avfFormats below are std::vector instantiations and need the
// complete type for size/destructor.
#include "../capture/IVideoCapture.h"
#endif

// Forward declarations
class UIManager;
class IVideoCapture;

/**
 * Source configuration window — was a tab inside the unified
 * "RetroCapture Controls" window; now stands alone, opened from
 * Configurations → Source on the main menu bar. Same render
 * contents as before; just gets its own ImGui::Begin/End +
 * visibility state.
 */
class UIConfigurationSource
{
public:
    UIConfigurationSource(UIManager *uiManager);
    ~UIConfigurationSource();

    void render();
    void setVisible(bool visible) { m_visible = visible; }
    bool isVisible() const        { return m_visible; }

private:
    bool m_visible = false;
    UIManager *m_uiManager = nullptr;
    IVideoCapture *m_capture = nullptr;

    void renderSourceTypeSelection();
    void renderV4L2Controls();
    void renderV4L2DeviceSelection();
#ifdef _WIN32
    void renderDSControls();
    void renderDSDeviceSelection();
#endif
#ifdef __APPLE__
    // macOS / AVFoundation: device dropdown + format dropdown
    // (OBS-style — picking a format selects width/height/fps/pixfmt
    // atomically since AVFoundation exposes them as fixed tuples).
    void renderAVFoundationControls();
    void renderAVFoundationDeviceSelection();
    void renderAVFoundationFormatSelection();

    // Per-frame cache of the device + format list so we don't hit
    // AVFoundation every frame from inside render().
    std::vector<DeviceInfo> m_avfDevices;
    std::vector<AVFoundationFormatInfo> m_avfFormats;
    bool m_avfDevicesNeedRefresh = true;
    bool m_avfFormatsNeedRefresh = true;
    std::string m_avfFormatsForDeviceId; // tracks which device's formats we cached
#endif
    void renderCaptureSettings();
    void renderQuickResolutions();
    void renderQuickFPS();
    // Phase 6+/#47 polish: UI surface for the Remote source — base URL
    // text input + Connect / Disconnect buttons that drive the existing
    // OnDeviceChanged callback flow in Application.
    void renderRemoteControls();

    // Persistent buffer for the URL ImGui input. Static-on-stack inside
    // render() would lose the typed value across re-renders, so keep it
    // here.
    char m_remoteUrlBuffer[256] = {0};
};
