#pragma once

#if defined(__linux__)
#  include "../output/VirtualCameraOutput.h"  // for DeviceInfo on the cache
#endif

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class UIManager;

/**
 * Configurations → Virtual Camera (#85).
 *
 * Standalone configuration window — sibling of Streaming /
 * Recording / Web Portal — that publishes RetroCapture's
 * processed framebuffer as a webcam consumers (OBS, Chrome,
 * Discord, Zoom, …) can pick up.
 *
 * - Linux (Phase 1): v4l2loopback device, with module load/remove
 *   helpers + a device picker.
 * - Windows (Phase 2): single shared-memory IPC channel consumed
 *   by RetroCaptureVCam.dll. No device picker; instead we surface
 *   whether the DirectShow filter DLL is registered.
 *
 * Owns the persistent UI state (the buffers + Linux's device-list
 * cache) but NOT the VirtualCameraOutput* sink itself — that
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

    void renderToggle();
    void renderOutputDims();
    void renderStatus();

#if defined(__linux__)
    // Cached enumeration. Refreshed on window-open and via the
    // "Rescan" button. Cheap (~ms) but we don't want to fire it
    // every frame either.
    std::vector<VirtualCameraOutput::DeviceInfo> m_deviceCache;
    bool m_deviceCacheLoaded = false;

    void renderDeviceSelector();
    void renderFormatSelector();
    void renderNoDeviceHelp();
    void renderModuleManagement();

    void refreshDevices();

    // Module load/remove via pkexec — async because the polkit
    // password prompt blocks. Worker thread fills `result` + flips
    // `done`; render() drains it next frame, surfaces the message,
    // and refreshes the device list on success.
    struct ModuleOp
    {
        std::atomic<bool> inFlight{false};
        std::atomic<bool> done{false};
        std::mutex        mu;
        VirtualCameraOutput::ModuleOpResult result;
        const char       *kind = "";   // "load" or "remove" — for status text
    };
    ModuleOp m_moduleOp;

    // Two-step confirm for the remove button — first click flips
    // this to true and the next render shows "Confirm remove?".
    bool m_removeConfirm = false;

    // Sticky status from the last completed pkexec call. Cleared
    // on the next op start.
    std::string m_moduleOpMessage;
    bool        m_moduleOpOk      = false;

    void submitLoadModule();
    void submitUnloadModule();
    void pumpModuleOp();
#endif // __linux__

#if defined(_WIN32) || defined(__APPLE__)
    // Windows + macOS path is simpler than Linux — one virtual
    // device, no device picker, no kernel module ops. The
    // "driver status" line is:
    //   - Windows: "is the DLL registered with COM" (regsvr32
    //     post-install).
    //   - macOS:  "is the DAL plug-in present in
    //     /Library/CoreMediaIO/Plug-Ins/DAL/" (install-virtcam.sh
    //     post-install).
    void renderDriverStatus();
#endif
};
