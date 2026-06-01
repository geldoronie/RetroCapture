#include "QuickActionsOverlay.h"
#include "OSDChat.h"
#include "../ui/UIManager.h"
#include "../ui/UIDirectoryBrowser.h"
#include "../ui/UISectionHeader.h"
#include "../utils/TranslationManager.h"
#if defined(__linux__)
#  include "../output/VirtualCameraOutput.h"
#endif

#include <imgui.h>

#include <cstdio>

QuickActionsOverlay::QuickActionsOverlay(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

QuickActionsOverlay::~QuickActionsOverlay() = default;

void QuickActionsOverlay::render()
{
    if (!m_visible || !m_uiManager) return;

    // Pinned to the bottom-right of the main viewport every frame.
    // The connection-status overlay shares this corner — it queries
    // renderedHeight() and shifts itself up to avoid overlap.
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const ImVec2 anchor(vp->WorkPos.x + vp->WorkSize.x - 16.0f,
                        vp->WorkPos.y + vp->WorkSize.y - 16.0f);
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(240, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    // No decoration. NoSavedSettings keeps imgui.ini clean — the
    // per-frame anchor is the only source of truth for position.
    // NoFocusOnAppearing avoids stealing clicks from underlying
    // capture when the overlay first paints.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                   ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_AlwaysAutoResize;

    if (!ImGui::Begin("##quickActions", nullptr, flags))
    {
        ImGui::End();
        return;
    }

    const bool clientMode      = m_uiManager->isRemoteSource() &&
                                 !m_uiManager->getCurrentDevice().empty();
    const bool streamingActive = m_uiManager->getStreamingActive();
    const bool recordingActive = m_uiManager->getRecordingActive();

    // Title — no native title bar, so a coloured label substitutes.
    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "%s",
                       T("quickactions.title").c_str());
    ImGui::Separator();

    // Status lines shown only when the matching session is active —
    // keeps the overlay compact instead of dedicating space to
    // greyed-out placeholders.
    if (!clientMode && streamingActive)
    {
        ui_status_bullet(ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
        ImGui::SameLine();
        ImGui::Text("Streaming");
        ImGui::SameLine();
        ImGui::TextDisabled("- %u %s",
                            m_uiManager->getStreamClientCount(),
                            T("quickactions.viewers").c_str());
    }
    // In client mode, show the host's viewer count instead. Fed by
    // RemoteMetaSync via Application — 0 means either we just
    // connected and /meta hasn't been parsed yet, or the host is
    // running an older build that doesn't emit streaming.clientCount.
    if (clientMode)
    {
        const uint32_t upstream = m_uiManager->getRemoteUpstreamClientCount();
        ui_status_bullet(ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
        ImGui::SameLine();
        ImGui::Text("Watching");
        ImGui::SameLine();
        ImGui::TextDisabled("- %u %s", upstream, T("quickactions.viewers").c_str());

        // #77 Audio volume + mute — mirrors UIRemoteConnection so the
        // user can ride the level from the OSD when the main UI is
        // hidden. Same UIManager triggers (persist + forward gain).
        bool  muted     = m_uiManager->getRemoteAudioMuted();
        float volumePct = m_uiManager->getRemoteAudioVolume() * 100.0f;
        if (ImGui::Checkbox(T("remote.audio_mute").c_str(), &muted))
        {
            m_uiManager->triggerRemoteAudioMuteChange(muted);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        ImGui::BeginDisabled(muted);
        if (ImGui::SliderFloat("##qaAudioVol", &volumePct, 0.0f, 100.0f, "%.0f%%"))
        {
            m_uiManager->triggerRemoteAudioVolumeChange(volumePct / 100.0f);
        }
        ImGui::EndDisabled();
    }
    if (recordingActive)
    {
        const uint64_t durationUs = m_uiManager->getRecordingDurationUs();
        uint64_t seconds = durationUs / 1000000;
        uint64_t minutes = seconds / 60;
        seconds %= 60;
        const uint64_t hours = minutes / 60;
        minutes %= 60;

        ui_status_bullet(ImVec4(1.0f, 0.30f, 0.30f, 1.0f));
        ImGui::SameLine();
        ImGui::Text("Recording");
        ImGui::SameLine();
        ImGui::TextDisabled("- %02llu:%02llu:%02llu",
                            static_cast<unsigned long long>(hours),
                            static_cast<unsigned long long>(minutes),
                            static_cast<unsigned long long>(seconds));
    }
    if ((!clientMode && streamingActive) || recordingActive)
    {
        ImGui::Separator();
    }

    // Open chat — flips the OSD chat overlay back on if the user
    // had hit the title-bar X. Cheap and always relevant; appears
    // first because it's the most-used action of the trio.
    if (ImGui::Button(T("quickactions.open_chat").c_str(), ImVec2(-1, 0)))
    {
        if (auto *chat = m_uiManager->getChatOverlay())
        {
            chat->setVisible(true);
        }
    }

    // Chat rooms — opens the OSDChat Rooms window (one-shot request
    // flag consumed by OSDChat next frame). Same widget the chat
    // panel's "Rooms..." header button uses, just from a different
    // entry point.
    if (ImGui::Button(T("quickactions.chat_rooms").c_str(), ImVec2(-1, 0)))
    {
        m_uiManager->requestOpenChatRooms();
    }

    // Browse streams (formerly "Browse directory"). Hidden while
    // we're streaming — exposing the directory browser to a host
    // who's currently live creates two confusing "viewer" UIs in
    // the same process. Joining someone else's stream while
    // broadcasting also breaks the capture pipeline. The user has
    // to stop the stream first; tooltip on the disabled button
    // explains why.
    if (UIDirectoryBrowser *browser = m_uiManager->getDirectoryBrowserWindow())
    {
        const bool blockedByStreaming = streamingActive && !clientMode;
        ImGui::BeginDisabled(blockedByStreaming);
        if (ImGui::Button(T("quickactions.browse").c_str(), ImVec2(-1, 0)))
        {
            browser->setVisible(true);
        }
        ImGui::EndDisabled();
        if (blockedByStreaming && ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s",
                T("quickactions.browse_disabled_streaming").c_str());
        }
    }

    // Disconnect — client mode only (no remote URL armed → nothing to
    // disconnect from).
    if (clientMode)
    {
        if (ImGui::Button(T("quickactions.disconnect").c_str(), ImVec2(-1, 0)))
        {
            m_uiManager->setCurrentDevice("");
        }
    }

    // Streaming button — mirror of UIConfigurationStreaming's
    // renderStartStopButton (processing → active → cooldown → idle).
    // Hidden in client mode (consuming a remote /raw — nothing
    // local to broadcast).
    if (!clientMode)
    {
        const bool processing = m_uiManager->isStreamingProcessing();
        const int64_t cooldownMs = m_uiManager->getStreamingCooldownRemainingMs();
        if (processing)
        {
            ImGui::BeginDisabled();
            ImGui::Button(streamingActive ? "Parando..." : "Iniciando...",
                          ImVec2(-1, 0));
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Aguarde o processo terminar");
            }
        }
        else if (streamingActive)
        {
            if (ImGui::Button(T("streaming.stop").c_str(), ImVec2(-1, 0)))
            {
                m_uiManager->setStreamingProcessing(true);
                m_uiManager->triggerStreamingStartStop(false);
            }
        }
        else if (cooldownMs > 0)
        {
            ImGui::BeginDisabled();
            char label[64];
            std::snprintf(label, sizeof(label), "Aguardando (%ds)",
                          static_cast<int>(cooldownMs / 1000));
            ImGui::Button(label, ImVec2(-1, 0));
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Aguarde o cooldown terminar antes de iniciar o streaming novamente");
            }
        }
        else
        {
            if (ImGui::Button(T("streaming.start").c_str(), ImVec2(-1, 0)))
            {
                m_uiManager->setStreamingProcessing(true);
                m_uiManager->triggerStreamingStartStop(true);
            }
        }
    }

    // Recording button — UIConfigurationRecording has no cooldown
    // wrapper, so the overlay mirrors the simpler shape. Stays
    // available in client mode (#68) since the video pipeline is
    // generic and records whatever's in the framebuffer; only audio
    // has a caveat called out in the tooltip.
    if (recordingActive)
    {
        if (ImGui::Button(T("recording.stop").c_str(), ImVec2(-1, 0)))
        {
            m_uiManager->triggerRecordingStartStop(false);
        }
    }
    else
    {
        if (ImGui::Button(T("recording.start").c_str(), ImVec2(-1, 0)))
        {
            m_uiManager->triggerRecordingStartStop(true);
        }
        if (clientMode && ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", T("quickactions.record_client.tip").c_str());
        }
    }

    // Virtual camera start/stop (#85, Linux only). Visible only
    // when the kernel module has at least one loopback device
    // available — otherwise the button would do nothing on
    // click. Mirrors the Streaming + Recording button shape so
    // it slots into the overlay's existing rhythm.
#if defined(__linux__)
    if (!clientMode)
    {
        // Cheap O(N) over a handful of /dev/video* nodes; runs
        // only when QuickActions is open, every frame. Could be
        // cached but the overhead is invisible in profiles.
        const bool haveLoopback =
            !VirtualCameraOutput::enumerateDevices().empty();
        if (haveLoopback)
        {
            const bool vcamOn = m_uiManager->getVirtcamEnabled();
            if (vcamOn)
            {
                if (ImGui::Button(T("quickactions.virtcam.stop").c_str(),
                                  ImVec2(-1, 0)))
                {
                    m_uiManager->setVirtcamEnabled(false);
                    m_uiManager->saveConfig();
                }
            }
            else
            {
                if (ImGui::Button(T("quickactions.virtcam.start").c_str(),
                                  ImVec2(-1, 0)))
                {
                    m_uiManager->setVirtcamEnabled(true);
                    m_uiManager->saveConfig();
                }
            }
        }
    }
#endif

    m_lastRenderedHeight = ImGui::GetWindowHeight();

    ImGui::End();
}
