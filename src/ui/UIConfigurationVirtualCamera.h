#pragma once

#include "../output/VirtualCameraOutput.h"  // for DeviceInfo on the cache

#include <string>
#include <vector>

class UIManager;

/**
 * Configurations → Virtual Camera (#85 Phase 1).
 *
 * Standalone configuration window — sibling of Streaming /
 * Recording / Web Portal — that publishes RetroCapture's
 * processed framebuffer to a v4l2loopback device on Linux so
 * downstream apps (OBS, Chrome, Discord, Zoom, …) can pick it
 * as a webcam source.
 *
 * Owns the persistent UI state (the buffers + the device-list
 * cache) but NOT the VirtualCameraOutput sink itself — that
 * lives on Application, driven by syncVirtualCamera() the same
 * way DirectoryClient is driven. The window is purely the
 * settings + status surface.
 */
class UIConfigurationVirtualCamera
{
public:
    UIConfigurationVirtualCamera(UIManager *uiManager);
    ~UIConfigurationVirtualCamera();

    void render();
    void setVisible(bool visible) { m_visible = visible; }
    bool isVisible() const        { return m_visible; }

private:
    UIManager *m_uiManager = nullptr;
    bool       m_visible   = false;

    // Cached enumeration. Refreshed on window-open and via the
    // "Rescan" button. Cheap (~ms) but we don't want to fire it
    // every frame either.
    std::vector<VirtualCameraOutput::DeviceInfo> m_deviceCache;
    bool m_deviceCacheLoaded = false;

    void renderToggle();
    void renderDeviceSelector();
    void renderOutputDims();
    void renderFormatSelector();
    void renderStatus();
    void renderNoDeviceHelp();

    void refreshDevices();
};
