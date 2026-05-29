#include "UIConfigurationVirtualCamera.h"

#include "UIManager.h"
#include "UISectionHeader.h"
#include "../utils/Logger.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <thread>

UIConfigurationVirtualCamera::UIConfigurationVirtualCamera(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UIConfigurationVirtualCamera::~UIConfigurationVirtualCamera() = default;

void UIConfigurationVirtualCamera::refreshDevices()
{
    m_deviceCache       = VirtualCameraOutput::enumerateDevices();
    m_deviceCacheLoaded = true;
}

void UIConfigurationVirtualCamera::render()
{
    if (!m_visible || !m_uiManager) return;

    // Drain any pkexec op that completed since last frame so the
    // result + refreshed device list are visible THIS frame.
    pumpModuleOp();

    // Lazy refresh on first paint. The user hits Rescan if they
    // modprobe'd v4l2loopback while the window was already open.
    if (!m_deviceCacheLoaded) refreshDevices();

    ImGui::SetNextWindowSize(ImVec2(540, 540), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Virtual Camera##virtcam", &m_visible))
    {
        ImGui::End();
        return;
    }

    ui_section_header("Virtual Camera",
                      "Publish the processed (post-shader, post-image) "
                      "output as a webcam other apps on this machine "
                      "can pick up. Linux uses v4l2loopback.");

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

    ImGui::End();
}

void UIConfigurationVirtualCamera::renderToggle()
{
    bool enabled = m_uiManager->getVirtcamEnabled();
    if (ImGui::Checkbox("Enable virtual camera", &enabled))
    {
        m_uiManager->setVirtcamEnabled(enabled);
        m_uiManager->saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip(
            "When on, RetroCapture pushes every rendered frame into\n"
            "the selected v4l2loopback device. OBS / Chrome / Discord\n"
            "/ Zoom will list it as a webcam.");
    }
}

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
        const auto r =
            VirtualCameraOutput::loadV4l2LoopbackModule("RetroCapture");
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
    std::thread([this]() {
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
