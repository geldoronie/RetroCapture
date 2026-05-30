#include "UIConfigurationVirtualCamera.h"

#include "UIManager.h"
#include "UISectionHeader.h"
#include "../utils/Logger.h"

#if defined(_WIN32)
#  include "../output/VirtualCameraOutputWin.h"
#endif

#include <imgui.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

UIConfigurationVirtualCamera::UIConfigurationVirtualCamera(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UIConfigurationVirtualCamera::~UIConfigurationVirtualCamera() = default;

#if defined(__linux__)
void UIConfigurationVirtualCamera::refreshDevices()
{
    m_deviceCache       = VirtualCameraOutput::enumerateDevices();
    m_deviceCacheLoaded = true;
}
#endif

void UIConfigurationVirtualCamera::render()
{
    if (!m_visible || !m_uiManager) return;

#if defined(__linux__)
    // Drain any pkexec op that completed since last frame so the
    // result + refreshed device list are visible THIS frame.
    pumpModuleOp();

    // Lazy refresh on first paint. The user hits Rescan if they
    // modprobe'd v4l2loopback while the window was already open.
    if (!m_deviceCacheLoaded) refreshDevices();
#endif

    ImGui::SetNextWindowSize(ImVec2(540, 540), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Virtual Camera##virtcam", &m_visible))
    {
        ImGui::End();
        return;
    }

    ui_section_header("Virtual Camera",
                      "Publish the processed (post-shader, post-image) "
                      "output as a webcam other apps on this machine "
                      "can pick up. Linux uses v4l2loopback; Windows "
                      "uses RetroCaptureVCam.dll (DirectShow filter).");

#if defined(__linux__)
    // Empty cache → install hint, no toggle.
    if (m_deviceCache.empty())
    {
        renderNoDeviceHelp();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        renderModuleManagement();
        ImGui::End();
        return;
    }

    renderToggle();
    ImGui::Spacing();
    renderDeviceSelector();
    renderOutputDims();
    renderFormatSelector();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    renderStatus();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    renderModuleManagement();
#elif defined(_WIN32)
    // Windows path: driver state up top, then the toggle (gated
    // off if the DLL isn't registered), output dims, and status.
    // No device picker — one CLSID, one virtual camera.
    renderDriverStatus();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const bool driverReady = VirtualCameraOutputWin::isFilterDllRegistered();
    if (!driverReady)
    {
        ImGui::BeginDisabled();
        renderToggle();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Register RetroCaptureVCam.dll first (see Driver above).");
        }
    }
    else
    {
        renderToggle();
    }

    ImGui::Spacing();
    renderOutputDims();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    renderStatus();
#endif

    ImGui::End();
}

void UIConfigurationVirtualCamera::renderToggle()
{
    // Explicit Start/Stop instead of a checkbox — mirrors the
    // streaming + recording UX so the user has the same mental
    // model across all three capture surfaces. Underneath it's
    // still the same boolean on UIManager that syncVirtualCamera
    // reconciles per frame; the button just flips it.
    const bool enabled = m_uiManager->getVirtcamEnabled();
    if (!enabled)
    {
        if (ImGui::Button("Start Capture", ImVec2(-1.0f, 0)))
        {
            m_uiManager->setVirtcamEnabled(true);
            m_uiManager->saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Start pushing the processed frame to the\n"
                "v4l2loopback device. OBS / Chrome / Discord /\n"
                "Zoom will list it as a webcam from this moment.");
        }
    }
    else
    {
        // Red-tinted stop button for symmetry with the chat
        // Disconnect treatment.
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.55f, 0.22f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.70f, 0.28f, 0.28f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.45f, 0.18f, 0.18f, 1.0f));
        if (ImGui::Button("Stop Capture", ImVec2(-1.0f, 0)))
        {
            m_uiManager->setVirtcamEnabled(false);
            m_uiManager->saveConfig();
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Release the v4l2loopback device. Consumers will\n"
                "see the camera as disconnected.");
        }
    }
}

#if defined(__linux__)
void UIConfigurationVirtualCamera::renderDeviceSelector()
{
    ImGui::TextDisabled("Device");
    ImGui::SetNextItemWidth(-90);

    // Current device path (configured). Empty means "auto-pick the
    // first loopback in the cache" — surface that in the dropdown
    // label so the user knows.
    const std::string &configured = m_uiManager->getVirtcamDevicePath();
    int selectedIndex = -1;
    if (configured.empty() && !m_deviceCache.empty())
    {
        selectedIndex = 0; // auto
    }
    else
    {
        for (size_t i = 0; i < m_deviceCache.size(); ++i)
        {
            if (m_deviceCache[i].path == configured)
            {
                selectedIndex = static_cast<int>(i);
                break;
            }
        }
        // Configured path no longer present (user unloaded the
        // module + reloaded with fewer devices). Fall back to
        // first available; persist will happen on next selection.
        if (selectedIndex < 0) selectedIndex = 0;
    }

    if (ImGui::BeginCombo("##virtcamDevice",
                          m_deviceCache[selectedIndex].cardLabel.c_str()))
    {
        for (size_t i = 0; i < m_deviceCache.size(); ++i)
        {
            const auto &d = m_deviceCache[i];
            char label[160];
            std::snprintf(label, sizeof(label), "%s (%s)",
                          d.cardLabel.c_str(), d.path.c_str());
            const bool isSel = (static_cast<int>(i) == selectedIndex);
            if (ImGui::Selectable(label, isSel))
            {
                m_uiManager->setVirtcamDevicePath(d.path);
                m_uiManager->saveConfig();
            }
            if (isSel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan", ImVec2(82, 0)))
    {
        refreshDevices();
    }
}
#endif // __linux__

void UIConfigurationVirtualCamera::renderOutputDims()
{
    // 0 = "follow shader output" — surfaced as a special hint
    // value rather than a checkbox so the user sees the numeric
    // override in one input.
    int w = static_cast<int>(m_uiManager->getVirtcamOutputWidth());
    int h = static_cast<int>(m_uiManager->getVirtcamOutputHeight());
    int f = static_cast<int>(m_uiManager->getVirtcamOutputFps());

    ImGui::TextDisabled("Output size (0 = follow shader output)");
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputInt("Width##virtcamW", &w))
    {
        if (w < 0) w = 0;
        if (w > 7680) w = 7680;
        m_uiManager->setVirtcamOutputWidth(static_cast<uint32_t>(w));
        m_uiManager->saveConfig();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputInt("Height##virtcamH", &h))
    {
        if (h < 0) h = 0;
        if (h > 4320) h = 4320;
        m_uiManager->setVirtcamOutputHeight(static_cast<uint32_t>(h));
        m_uiManager->saveConfig();
    }

    ImGui::TextDisabled("Declared FPS (0 = follow capture)");
    ImGui::SetNextItemWidth(110);
    if (ImGui::InputInt("FPS##virtcamF", &f))
    {
        if (f < 0)   f = 0;
        if (f > 240) f = 240;
        m_uiManager->setVirtcamOutputFps(static_cast<uint32_t>(f));
        m_uiManager->saveConfig();
    }
}

#if defined(__linux__)
void UIConfigurationVirtualCamera::renderFormatSelector()
{
    // Persisted as a string for round-trip simplicity; the sink
    // (VirtualCameraOutput::PixelFormat) maps from it. Default
    // YUYV matches what every meeting app expects.
    const std::string &cur = m_uiManager->getVirtcamPixelFormat();
    const char *items[]    = { "YUYV (max compatibility)", "RGB24 (no chroma loss)" };
    const char *values[]   = { "yuyv", "rgb24" };
    int curIdx = 0;
    if (cur == "rgb24") curIdx = 1;

    ImGui::TextDisabled("Pixel format");
    ImGui::SetNextItemWidth(240);
    if (ImGui::Combo("##virtcamFmt", &curIdx, items, 2))
    {
        m_uiManager->setVirtcamPixelFormat(values[curIdx]);
        m_uiManager->saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "YUYV is the safe default — every browser / meeting app\n"
            "consumes it. RGB24 skips the chroma subsampling step\n"
            "and is slightly cheaper CPU-wise but some consumers\n"
            "(notably browsers via getUserMedia) may reject it.");
    }
}
#endif // __linux__

void UIConfigurationVirtualCamera::renderStatus()
{
    ui_section_header("Status");
    // Pull live status from UIManager — populated by Application
    // after each syncVirtualCamera tick.
    const std::string &line = m_uiManager->getVirtcamStatusText();
    const std::string &err  = m_uiManager->getVirtcamErrorText();
    if (!err.empty())
    {
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f),
                           "Error: %s", err.c_str());
    }
    if (line.empty())
    {
        ImGui::TextDisabled("Idle.");
    }
    else
    {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.50f, 1.0f),
                           "%s", line.c_str());
    }
}

#if defined(__linux__)
void UIConfigurationVirtualCamera::renderNoDeviceHelp()
{
    ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.30f, 1.0f),
                       "No v4l2loopback device found.");
    ImGui::Spacing();
    ImGui::TextWrapped(
        "RetroCapture's virtual camera writes to a v4l2loopback "
        "kernel module device. The kernel module either isn't "
        "loaded right now or no loopback devices were created.");
    ImGui::Spacing();
    ImGui::TextDisabled(
        "If the package isn't installed yet, install it once:");
    ImGui::TextWrapped("  sudo apt install v4l2loopback-dkms    "
                       "(Debian / Ubuntu)");
    ImGui::TextWrapped("  sudo pacman -S v4l2loopback-dkms      "
                       "(Arch)");
}

// ---- Module management section (load / remove via pkexec) -----------

void UIConfigurationVirtualCamera::submitLoadModule()
{
    if (m_moduleOp.inFlight.exchange(true)) return;
    m_moduleOp.kind = "load";
    // Hardcoded "RetroCapture" card label so consumers see a
    // friendly name. The sanitiser in VirtualCameraOutput will
    // pass it through unchanged.
    std::thread([this]() {
        auto r = VirtualCameraOutput::loadV4l2LoopbackModule("RetroCapture");
        // udev creates /dev/videoN asynchronously after modprobe
        // returns — wait up to ~600 ms for the node to appear so
        // pumpModuleOp's refreshDevices() actually sees it. Cap
        // the loop tight; if udev never delivers (containerised
        // environment, missing udev rules) we still finish and
        // report success — the UI just stays empty.
        if (r.ok)
        {
            for (int i = 0; i < 30; ++i)
            {
                if (!VirtualCameraOutput::enumerateDevices().empty())
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        {
            std::lock_guard<std::mutex> lk(m_moduleOp.mu);
            m_moduleOp.result = r;
        }
        m_moduleOp.done.store(true);
        m_moduleOp.inFlight.store(false);
    }).detach();
}

void UIConfigurationVirtualCamera::submitUnloadModule()
{
    if (m_moduleOp.inFlight.exchange(true)) return;
    m_moduleOp.kind = "remove";

    // Tell Application to stop the sink (and clear the user
    // toggle so syncVirtualCamera doesn't re-open after rmmod).
    // The worker spins on virtcamStopped before launching pkexec
    // — without this, rmmod fails with EBUSY because we're still
    // holding the device's fd.
    m_uiManager->setVirtcamEnabled(false);
    m_uiManager->saveConfig();
    m_uiManager->setVirtcamStopped(false);
    m_uiManager->requestVirtcamStop();

    UIManager *ui = m_uiManager;
    std::thread([this, ui]() {
        // Poll up to ~500 ms (50 × 10 ms) for the sink to release
        // the device. The render thread typically picks up the
        // request within 1-2 frames; the timeout is generous so a
        // briefly-stalled render loop doesn't dump the user into
        // an EBUSY error.
        for (int i = 0; i < 50; ++i)
        {
            if (ui->isVirtcamStopped()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto r = VirtualCameraOutput::unloadV4l2LoopbackModule();
        {
            std::lock_guard<std::mutex> lk(m_moduleOp.mu);
            m_moduleOp.result = r;
        }
        m_moduleOp.done.store(true);
        m_moduleOp.inFlight.store(false);
    }).detach();
}

void UIConfigurationVirtualCamera::pumpModuleOp()
{
    if (!m_moduleOp.done.exchange(false)) return;
    std::lock_guard<std::mutex> lk(m_moduleOp.mu);
    m_moduleOpOk      = m_moduleOp.result.ok;
    m_moduleOpMessage = m_moduleOp.result.message;
    // Device list almost certainly changed — refresh.
    refreshDevices();
    // If the user just removed the module and the sink is still
    // running, the next syncVirtualCamera tick on Application
    // will notice the device is gone and stop the sink. Nothing
    // to do here.
    // The two-step confirm arms for one click only; reset.
    m_removeConfirm = false;
}

void UIConfigurationVirtualCamera::renderModuleManagement()
{
    ui_section_header("Module management",
                      "Load or remove the v4l2loopback kernel "
                      "module. Requires polkit (pkexec) for the "
                      "elevation prompt.");

    const bool hasPkexec = VirtualCameraOutput::pkexecAvailable();
    const bool haveDevs  = !m_deviceCache.empty();
    const bool busy      = m_moduleOp.inFlight.load();

    if (!hasPkexec)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.30f, 1.0f),
            "pkexec not found — install the polkit package to "
            "enable one-click load/remove. Until then, run "
            "modprobe / rmmod manually.");
        // Still expose the modprobe command for copy-paste in
        // case the user prefers it.
        const char *modprobeCmd =
            "sudo modprobe v4l2loopback exclusive_caps=1 "
            "card_label=\"RetroCapture\"";
        ImGui::TextDisabled("Load command:");
        ImGui::TextWrapped("  %s", modprobeCmd);
        if (ImGui::Button("Copy load command"))
        {
            ImGui::SetClipboardText(modprobeCmd);
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy remove command"))
        {
            ImGui::SetClipboardText("sudo rmmod v4l2loopback");
        }
        ImGui::SameLine();
        if (ImGui::Button("Rescan")) refreshDevices();
        return;
    }

    // Load module — visible always; disabled while op in flight or
    // when at least one device already exists (modprobe a second
    // time would no-op).
    ImGui::BeginDisabled(busy || haveDevs);
    if (ImGui::Button("Install v4l2loopback device"))
    {
        m_moduleOpMessage.clear();
        submitLoadModule();
    }
    ImGui::EndDisabled();
    if (haveDevs && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "Module is already loaded. Remove it first if you "
            "want to reload with different parameters.");
    }
    ImGui::SameLine();

    // Remove module — visible only when devices exist. Two-step
    // confirm because it disconnects any consumer (OBS / Chrome
    // / Discord) currently using the device.
    ImGui::BeginDisabled(busy || !haveDevs);
    if (m_removeConfirm)
    {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(0.70f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(0.85f, 0.25f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("Confirm remove"))
        {
            m_moduleOpMessage.clear();
            submitUnloadModule();
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("Cancel##rmcancel"))
        {
            m_removeConfirm = false;
        }
    }
    else
    {
        if (ImGui::Button("Remove module"))
        {
            m_removeConfirm = true;
        }
    }
    ImGui::EndDisabled();
    if (!haveDevs && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Module isn't loaded; nothing to remove.");
    }

    ImGui::SameLine();
    if (ImGui::Button("Rescan##modules"))
    {
        refreshDevices();
    }

    // Status line for the last completed op + spinner while busy.
    if (busy)
    {
        ImGui::Spacing();
        ImGui::TextDisabled("Working... (polkit may be asking for "
                            "your password)");
    }
    if (!m_moduleOpMessage.empty())
    {
        ImGui::Spacing();
        const ImVec4 col = m_moduleOpOk
            ? ImVec4(0.45f, 0.85f, 0.50f, 1.0f)
            : ImVec4(0.95f, 0.35f, 0.35f, 1.0f);
        ImGui::TextColored(col, "%s", m_moduleOpMessage.c_str());
    }
}
#endif // __linux__

#if defined(_WIN32)
void UIConfigurationVirtualCamera::renderDriverStatus()
{
    // The DirectShow filter DLL is registered post-install by
    // NSIS's regsvr32 step. If it's missing the user can't toggle
    // capture on — explain how to fix it.
    const bool registered = VirtualCameraOutputWin::isFilterDllRegistered();

    ImGui::TextDisabled("Driver");
    if (registered)
    {
        ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.45f, 1.0f),
                           "Registered — RetroCaptureVCam.dll is "
                           "available to consumers (OBS, Chrome, …).");
    }
    else
    {
        ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.30f, 1.0f),
                           "Not registered — RetroCaptureVCam.dll "
                           "is missing from the COM registry.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Run the RetroCapture installer (it registers the DLL "
            "via regsvr32 in its post-install step). If you're on a "
            "portable / non-installed build, open an Administrator "
            "command prompt in the install folder and run:");
        ImGui::Spacing();
        ImGui::TextDisabled("    regsvr32 RetroCaptureVCam.dll");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Uninstall later with: regsvr32 /u RetroCaptureVCam.dll");
    }
}
#endif // _WIN32
