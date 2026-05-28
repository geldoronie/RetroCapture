#include "UIConfigurationStreaming.h"
#include "UIManager.h"
#include "UISectionHeader.h"
#include "../identity/OwnedRooms.h"
#include "../utils/Logger.h"
#include "../utils/TranslationManager.h"
#include "../encoding/MediaEncoder.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <thread>

namespace
{
    // #56 — minimal URL validation for the "Custom" endpoint mode.
    // Catches the typo class that used to leak into the directory
    // ("htts://foo", "https//foo.com", missing port number etc.) and
    // makes the receiving service look broken to anyone browsing.
    // The companion guard on the service side rejects malformed
    // endpoints with 400, but the inline UI check catches the typo
    // before the user toggles Publish on at all.
    bool validateCustomEndpoint(const std::string &url, std::string &errOut)
    {
        errOut.clear();
        if (url.empty())
        {
            errOut = "URL is empty";
            return false;
        }
        size_t schemeEnd;
        if (url.rfind("http://", 0) == 0)       schemeEnd = 7;
        else if (url.rfind("https://", 0) == 0) schemeEnd = 8;
        else
        {
            errOut = "URL must start with http:// or https://";
            return false;
        }
        size_t i         = schemeEnd;
        size_t hostStart = i;
        while (i < url.size() && url[i] != ':' && url[i] != '/') ++i;
        const std::string host = url.substr(hostStart, i - hostStart);
        if (host.empty())
        {
            errOut = "Host is empty";
            return false;
        }
        for (char c : host)
        {
            if (!(std::isalnum(static_cast<unsigned char>(c)) ||
                  c == '.' || c == '-'))
            {
                errOut = "Invalid character in host";
                return false;
            }
        }
        // Host must contain at least one alphanumeric (eg. "..." or
        // "---" alone is meaningless).
        bool anyAlnum = false;
        for (char c : host)
        {
            if (std::isalnum(static_cast<unsigned char>(c))) { anyAlnum = true; break; }
        }
        if (!anyAlnum)
        {
            errOut = "Host must contain a letter or digit";
            return false;
        }
        if (i < url.size() && url[i] == ':')
        {
            ++i;
            const size_t portStart = i;
            while (i < url.size() && url[i] != '/') ++i;
            const std::string portStr = url.substr(portStart, i - portStart);
            if (portStr.empty())
            {
                errOut = "Port is empty";
                return false;
            }
            long port = 0;
            try { port = std::stol(portStr); }
            catch (...) { errOut = "Invalid port"; return false; }
            for (char c : portStr)
            {
                if (!std::isdigit(static_cast<unsigned char>(c)))
                {
                    errOut = "Invalid port";
                    return false;
                }
            }
            if (port < 1 || port > 65535)
            {
                errOut = "Port out of range (1..65535)";
                return false;
            }
        }
        // Anything after the host[:port] is the path — accepted as-is.
        return true;
    }
}

UIConfigurationStreaming::UIConfigurationStreaming(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UIConfigurationStreaming::~UIConfigurationStreaming()
{
}

void UIConfigurationStreaming::render()
{
    if (!m_visible || !m_uiManager) return;

    ImGui::SetNextWindowSize(ImVec2(680, 720), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("streaming.title").c_str(), &m_visible))
    {
        ImGui::End();
        return;
    }

    renderStreamingStatus();
    renderProfiles();
    {
        ui_section_header("Pipeline",
                          "Choose which feed goes on the wire: the raw "
                          "source, or the post-shader output.");
        bool apply = m_uiManager->getStreamingApplyShader();
        if (ImGui::Checkbox("Apply shader to stream", &apply))
        {
            m_uiManager->setStreamingApplyShader(apply);
            m_uiManager->saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("When off, the stream pushes the raw pre-shader source\n"
                              "even though the live preview shows the shader applied.\n"
                              "Useful to broadcast a clean feed while keeping the CRT\n"
                              "look on screen.");
        }
    }
    renderBasicSettings();
    renderCodecSettings();
    renderBitrateSettings();
    renderDirectoryPublish();      // #49 Phase 2
    // Buffer tuning (max video/audio buffer, max buffer time, AVIO buffer)
    // is not surfaced in the UI anymore — defaults work for the vast
    // majority of cases. Power users can still override via config.json.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    renderStartStopButton();

    ImGui::End();
}

void UIConfigurationStreaming::renderStreamingStatus()
{
    ui_section_header("HTTP MPEG-TS Streaming (audio + video)");
    bool active = m_uiManager->getStreamingActive();
    ui_status_indicator(active, "Active", "Inactive");

    if (active)
    {
        std::string url = m_uiManager->getStreamUrl();
        if (!url.empty())
        {
            ImGui::Text("URL: %s", url.c_str());
        }
        ImGui::Text("Clientes conectados: %u", m_uiManager->getStreamClientCount());
    }
}

void UIConfigurationStreaming::refreshProfiles()
{
    m_profileNames = m_uiManager->listStreamingProfiles();
    m_profilesDirty = false;
    if (m_selectedProfileIndex >= static_cast<int>(m_profileNames.size()))
    {
        m_selectedProfileIndex = m_profileNames.empty() ? -1 : 0;
    }
}

void UIConfigurationStreaming::renderProfiles()
{
    if (m_profilesDirty) refreshProfiles();

    ui_section_header("Profiles",
                      "Saved encoder configurations you can swap "
                      "between.");

    const char *currentLabel = (m_selectedProfileIndex >= 0 &&
                                m_selectedProfileIndex < static_cast<int>(m_profileNames.size()))
                                   ? m_profileNames[m_selectedProfileIndex].c_str()
                                   : "(no profile selected)";

    if (ImGui::BeginCombo("##streaming_profile", currentLabel))
    {
        for (int i = 0; i < static_cast<int>(m_profileNames.size()); ++i)
        {
            bool selected = (i == m_selectedProfileIndex);
            if (ImGui::Selectable(m_profileNames[i].c_str(), selected))
            {
                m_selectedProfileIndex = i;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        if (m_profileNames.empty())
        {
            ImGui::TextDisabled("(no profiles saved)");
        }
        ImGui::EndCombo();
    }

    bool hasSelection = (m_selectedProfileIndex >= 0 &&
                         m_selectedProfileIndex < static_cast<int>(m_profileNames.size()));

    if (!hasSelection) ImGui::BeginDisabled();
    if (ImGui::Button("Apply"))
    {
        const std::string &name = m_profileNames[m_selectedProfileIndex];
        if (!m_uiManager->loadStreamingProfile(name))
        {
            LOG_ERROR("Failed to load streaming profile: " + name);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete"))
    {
        m_showDeleteConfirm = true;
    }
    if (!hasSelection) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Save as..."))
    {
        m_newProfileName[0] = '\0';
        m_showSaveDialog = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
    {
        m_profilesDirty = true;
    }

    if (m_showSaveDialog) ImGui::OpenPopup("Save Streaming Profile");
    if (ImGui::BeginPopupModal("Save Streaming Profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Profile name:");
        ImGui::InputText("##streaming_profile_name", m_newProfileName, sizeof(m_newProfileName));

        std::string nameStr(m_newProfileName);
        bool exists = !nameStr.empty() && m_uiManager->streamingProfileExists(nameStr);
        if (exists)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "A profile with this name already exists.");
        }

        if (ImGui::Button("Save", ImVec2(120, 0)))
        {
            if (!nameStr.empty())
            {
                if (m_uiManager->saveStreamingProfile(nameStr))
                {
                    m_profilesDirty = true;
                    m_showSaveDialog = false;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            m_showSaveDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (m_showDeleteConfirm) ImGui::OpenPopup("Delete Streaming Profile");
    if (ImGui::BeginPopupModal("Delete Streaming Profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (m_selectedProfileIndex >= 0 && m_selectedProfileIndex < static_cast<int>(m_profileNames.size()))
        {
            ImGui::Text("Delete profile \"%s\"?", m_profileNames[m_selectedProfileIndex].c_str());
        }
        if (ImGui::Button("Delete", ImVec2(120, 0)))
        {
            if (m_selectedProfileIndex >= 0 && m_selectedProfileIndex < static_cast<int>(m_profileNames.size()))
            {
                m_uiManager->deleteStreamingProfile(m_profileNames[m_selectedProfileIndex]);
                m_profilesDirty = true;
                m_selectedProfileIndex = -1;
            }
            m_showDeleteConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            m_showDeleteConfirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void UIConfigurationStreaming::renderBasicSettings()
{
    ui_section_header("Video",
                      "Output resolution and frame rate sent on the "
                      "wire. \"Capture\" forwards whatever the source "
                      "delivers.");

    // Porta
    int port = static_cast<int>(m_uiManager->getStreamingPort());
    if (ImGui::InputInt("Port", &port, 1, 100))
    {
        if (port >= 1024 && port <= 65535)
        {
            m_uiManager->triggerStreamingPortChange(static_cast<uint16_t>(port));
        }
    }

    // Resolução - Dropdown
    const char *resolutions[] = {
        "Captura (0x0)",
        "320x240",
        "640x480",
        "800x600",
        "1024x768",
        "1280x720 (HD)",
        "1280x1024",
        "1920x1080 (Full HD)",
        "2560x1440 (2K)",
        "3840x2160 (4K)"};
    const uint32_t resolutionWidths[] = {0, 320, 640, 800, 1024, 1280, 1280, 1920, 2560, 3840};
    const uint32_t resolutionHeights[] = {0, 240, 480, 600, 768, 720, 1024, 1080, 1440, 2160};

    uint32_t currentWidth = m_uiManager->getStreamingWidth();
    uint32_t currentHeight = m_uiManager->getStreamingHeight();
    int currentResIndex = 0;
    for (int i = 0; i < 10; i++)
    {
        if (currentWidth == resolutionWidths[i] && currentHeight == resolutionHeights[i])
        {
            currentResIndex = i;
            break;
        }
    }

    if (ImGui::Combo("Resolution", &currentResIndex, resolutions, 10))
    {
        m_uiManager->triggerStreamingWidthChange(resolutionWidths[currentResIndex]);
        m_uiManager->triggerStreamingHeightChange(resolutionHeights[currentResIndex]);
    }

    // FPS - Dropdown
    const char *fpsOptions[] = {"Captura (0)", "15", "24", "30", "60", "120"};
    const uint32_t fpsValues[] = {0, 15, 24, 30, 60, 120};

    uint32_t currentFps = m_uiManager->getStreamingFps();
    int currentFpsIndex = 0;
    for (int i = 0; i < 6; i++)
    {
        if (currentFps == fpsValues[i])
        {
            currentFpsIndex = i;
            break;
        }
    }

    if (ImGui::Combo("FPS", &currentFpsIndex, fpsOptions, 6))
    {
        m_uiManager->triggerStreamingFpsChange(fpsValues[currentFpsIndex]);
    }
}

void UIConfigurationStreaming::renderCodecSettings()
{
    ui_section_header("Codecs",
                      "What encodes the video and audio. Codec-specific "
                      "tuning appears below the dropdown.");

    // Seleção de codec de vídeo
    const char *videoCodecs[] = {"h264", "h265", "vp8", "vp9"};
    std::string currentVideoCodec = m_uiManager->getStreamingVideoCodec();
    int currentVideoCodecIndex = 0;
    for (int i = 0; i < 4; i++)
    {
        if (currentVideoCodec == videoCodecs[i])
        {
            currentVideoCodecIndex = i;
            break;
        }
    }

    if (ImGui::Combo("Video Codec", &currentVideoCodecIndex, videoCodecs, 4))
    {
        m_uiManager->triggerStreamingVideoCodecChange(videoCodecs[currentVideoCodecIndex]);
    }

    // Hardware encoder dropdown — populated from what ffmpeg was built
    // with on this host. Auto always present; Software always present;
    // the hardware backends only show up when the corresponding codec
    // (h264_nvenc, h264_vaapi, …) is compiled into the linked ffmpeg.
    // Relevant for both H.264 and H.265 — the other codecs in this
    // project (vp8/vp9) have no hardware equivalents we support.
    if (currentVideoCodec == "h264" || currentVideoCodec == "h265" || currentVideoCodec == "hevc")
    {
        std::vector<MediaEncoder::HardwareEncoder> available = MediaEncoder::detectAvailableEncoders();
        // Prepend Auto so the user can let the app pick.
        std::vector<MediaEncoder::HardwareEncoder> options;
        options.push_back(MediaEncoder::HardwareEncoder::Auto);
        for (auto h : available) options.push_back(h);

        std::vector<const char *> labels;
        labels.reserve(options.size());
        for (auto h : options) labels.push_back(MediaEncoder::hardwareEncoderName(h));

        int current = m_uiManager->getStreamingHardwareEncoder();
        int currentIndex = 0;
        for (size_t i = 0; i < options.size(); ++i)
        {
            if (static_cast<int>(options[i]) == current) { currentIndex = static_cast<int>(i); break; }
        }
        if (ImGui::Combo("Encoder", &currentIndex, labels.data(), static_cast<int>(labels.size())))
        {
            m_uiManager->triggerStreamingHardwareEncoderChange(static_cast<int>(options[currentIndex]));
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Auto = try hardware (NVENC/VAAPI/QSV/AMF), fall back to software on failure.\n"
                              "Software = libx264 guaranteed on any machine.\n"
                              "Hardware backends only show up when ffmpeg was built with support and\n"
                              "may fail at runtime if the driver/permission is missing — in that\n"
                              "case the stream falls back to libx264 automatically.");
        }

        // Backend-specific quality / rate-control combo. libx264 keeps
        // its existing "H.264 Quality" dropdown rendered below;
        // hardware backends each expose the parameter that matters most
        // for them. Auto is treated as software-fallback for UX purposes
        // — the actual backend is resolved at codec-open time anyway.
        MediaEncoder::HardwareEncoder activeHw = options[currentIndex];
        auto renderEnumCombo = [&](const char *label, const char *const *items, int itemCount,
                                   const std::string &currentValue,
                                   std::function<void(const std::string &)> onChange,
                                   const char *tooltip)
        {
            int idx = 0;
            for (int i = 0; i < itemCount; ++i)
            {
                if (currentValue == items[i]) { idx = i; break; }
            }
            if (ImGui::Combo(label, &idx, items, itemCount))
            {
                onChange(items[idx]);
            }
            if (tooltip && ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", tooltip);
            }
        };

        if (activeHw == MediaEncoder::HardwareEncoder::NVENC)
        {
            static const char *items[] = {"p1", "p2", "p3", "p4", "p5", "p6", "p7"};
            renderEnumCombo("NVENC Preset", items, 7, m_uiManager->getStreamingNvencPreset(),
                            [this](const std::string &v) { m_uiManager->triggerStreamingNvencPresetChange(v); },
                            "p1 = fastest (lowest quality) ... p7 = slowest (highest quality).\n"
                            "p4 is the recommended balance for real-time streaming.");
        }
        else if (activeHw == MediaEncoder::HardwareEncoder::VAAPI)
        {
            static const char *items[] = {"CBR", "VBR", "CQP"};
            renderEnumCombo("VAAPI Rate Control", items, 3, m_uiManager->getStreamingVaapiRcMode(),
                            [this](const std::string &v) { m_uiManager->triggerStreamingVaapiRcModeChange(v); },
                            "CBR = constant bitrate (recommended for streaming).\n"
                            "VBR = variable bitrate (better for recording).\n"
                            "CQP = constant quality (ignores bitrate).");
        }
        else if (activeHw == MediaEncoder::HardwareEncoder::QSV)
        {
            static const char *items[] = {"veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"};
            renderEnumCombo("QSV Preset", items, 7, m_uiManager->getStreamingQsvPreset(),
                            [this](const std::string &v) { m_uiManager->triggerStreamingQsvPresetChange(v); },
                            "Intel Quick Sync presets. Faster = lower quality.\n"
                            "veryfast/faster are recommended for real-time.");
        }
        else if (activeHw == MediaEncoder::HardwareEncoder::AMF)
        {
            static const char *items[] = {"speed", "balanced", "quality"};
            renderEnumCombo("AMF Quality", items, 3, m_uiManager->getStreamingAmfQuality(),
                            [this](const std::string &v) { m_uiManager->triggerStreamingAmfQualityChange(v); },
                            "speed = minimum latency, basic quality.\n"
                            "balanced = middle ground.\n"
                            "quality = best visual, higher latency.");
        }
    }

    // Seleção de codec de áudio
    const char *audioCodecs[] = {"aac", "mp3", "opus"};
    std::string currentAudioCodec = m_uiManager->getStreamingAudioCodec();
    int currentAudioCodecIndex = 0;
    for (int i = 0; i < 3; i++)
    {
        if (currentAudioCodec == audioCodecs[i])
        {
            currentAudioCodecIndex = i;
            break;
        }
    }

    if (ImGui::Combo("Codec de Áudio", &currentAudioCodecIndex, audioCodecs, 3))
    {
        m_uiManager->triggerStreamingAudioCodecChange(audioCodecs[currentAudioCodecIndex]);
    }

    // Qualidade H.264 (apenas se codec for h264)
    if (currentVideoCodec == "h264")
    {
        renderH264Settings();
    }

    // Qualidade H.265 (apenas se codec for h265)
    if (currentVideoCodec == "h265" || currentVideoCodec == "hevc")
    {
        renderH265Settings();
    }

    // Configurações VP8 (apenas se codec for vp8)
    if (currentVideoCodec == "vp8")
    {
        renderVP8Settings();
    }

    // Configurações VP9 (apenas se codec for vp9)
    if (currentVideoCodec == "vp9")
    {
        renderVP9Settings();
    }
}

void UIConfigurationStreaming::renderH264Settings()
{
    const char *h264Presets[] = {
        "ultrafast",
        "superfast",
        "veryfast",
        "faster",
        "fast",
        "medium",
        "slow",
        "slower",
        "veryslow"};
    std::string currentPreset = m_uiManager->getStreamingH264Preset();
    int currentPresetIndex = 2; // Padrão: veryfast
    for (int i = 0; i < 9; i++)
    {
        if (currentPreset == h264Presets[i])
        {
            currentPresetIndex = i;
            break;
        }
    }

    if (ImGui::Combo("H.264 Quality", &currentPresetIndex, h264Presets, 9))
    {
        m_uiManager->triggerStreamingH264PresetChange(h264Presets[currentPresetIndex]);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("H.264 encoder preset:\n"
                          "ultrafast/superfast/veryfast: maximum speed, lower quality\n"
                          "fast/medium: balance between speed and quality\n"
                          "slow/slower/veryslow: maximum quality, lower speed");
    }
}

void UIConfigurationStreaming::renderH265Settings()
{
    const char *h265Presets[] = {
        "ultrafast",
        "superfast",
        "veryfast",
        "faster",
        "fast",
        "medium",
        "slow",
        "slower",
        "veryslow"};
    std::string currentPreset = m_uiManager->getStreamingH265Preset();
    int currentPresetIndex = 2; // Padrão: veryfast
    for (int i = 0; i < 9; i++)
    {
        if (currentPreset == h265Presets[i])
        {
            currentPresetIndex = i;
            break;
        }
    }

    if (ImGui::Combo("H.265 Quality", &currentPresetIndex, h265Presets, 9))
    {
        m_uiManager->triggerStreamingH265PresetChange(h265Presets[currentPresetIndex]);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("H.265 encoder preset:\n"
                          "ultrafast/superfast/veryfast: maximum speed, lower quality\n"
                          "fast/medium: balance between speed and quality\n"
                          "slow/slower/veryslow: maximum quality, lower speed");
    }

    // Profile H.265
    const char *h265Profiles[] = {"main", "main10"};
    std::string currentProfile = m_uiManager->getStreamingH265Profile();
    int currentProfileIndex = 0;
    for (int i = 0; i < 2; i++)
    {
        if (currentProfile == h265Profiles[i])
        {
            currentProfileIndex = i;
            break;
        }
    }

    if (ImGui::Combo("H.265 Profile", &currentProfileIndex, h265Profiles, 2))
    {
        m_uiManager->triggerStreamingH265ProfileChange(h265Profiles[currentProfileIndex]);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("H.265 encoder profile:\n"
                          "main: 8-bit, maximum compatibility\n"
                          "main10: 10-bit, better quality, HDR support");
    }

    // Level H.265
    const char *h265Levels[] = {
        "auto", "1", "2", "2.1", "3", "3.1",
        "4", "4.1", "5", "5.1", "5.2", "6", "6.1", "6.2"};
    std::string currentLevel = m_uiManager->getStreamingH265Level();
    int currentLevelIndex = 0;
    for (int i = 0; i < 14; i++)
    {
        if (currentLevel == h265Levels[i])
        {
            currentLevelIndex = i;
            break;
        }
    }

    if (ImGui::Combo("H.265 Level", &currentLevelIndex, h265Levels, 14))
    {
        m_uiManager->triggerStreamingH265LevelChange(h265Levels[currentLevelIndex]);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("H.265 encoder level:\n"
                          "auto: automatic detection (recommended)\n"
                          "1-6.2: specific levels for compatibility\n"
                          "Higher levels support larger resolutions/bitrates");
    }
}

void UIConfigurationStreaming::renderVP8Settings()
{
    int currentSpeed = m_uiManager->getStreamingVP8Speed();
    if (ImGui::SliderInt("VP8 Speed (0-16)", &currentSpeed, 0, 16))
    {
        m_uiManager->triggerStreamingVP8SpeedChange(currentSpeed);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("VP8 encoder speed:\n"
                          "0: best quality, slower\n"
                          "16: faster, lower quality\n"
                          "12: good balance for streaming");
    }
}

void UIConfigurationStreaming::renderVP9Settings()
{
    int currentSpeed = m_uiManager->getStreamingVP9Speed();
    if (ImGui::SliderInt("VP9 Speed (0-9)", &currentSpeed, 0, 9))
    {
        m_uiManager->triggerStreamingVP9SpeedChange(currentSpeed);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("VP9 encoder speed:\n"
                          "0: best quality, slower\n"
                          "9: faster, lower quality\n"
                          "6: good balance for streaming");
    }
}

void UIConfigurationStreaming::renderBitrateSettings()
{
    ui_section_header("Bitrate",
                      "How many bits per second the encoder spends on "
                      "video and audio.");

    // Same SliderFloat presentation as UIConfigurationRecording — the
    // two windows share visual idiom AND interaction grammar so the
    // user doesn't have to type numbers in one place and drag in the
    // other. Storage on the streaming side is in kbps (not bps like
    // Recording), so the Mbps↔kbps conversion just uses a factor of
    // 1000 instead of 1_000_000.
    uint32_t bitrate = m_uiManager->getStreamingBitrate();             // kbps
    float bitrateMbps = static_cast<float>(bitrate) / 1000.0f;
    if (ImGui::SliderFloat("Video Bitrate (Mbps)", &bitrateMbps, 1.0f, 50.0f, "%.1f"))
    {
        m_uiManager->triggerStreamingBitrateChange(static_cast<uint32_t>(bitrateMbps * 1000.0f));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Recommended starting points:\n"
                          " ~3 Mbps  — 720p\n"
                          " ~6 Mbps  — 1080p\n"
                          " ~12 Mbps — 1080p HEVC at zero-compromise\n"
                          " ~25 Mbps — 4K");
    }

    uint32_t audioBitrate = m_uiManager->getStreamingAudioBitrate();  // already kbps
    float audioBitrateKbps = static_cast<float>(audioBitrate);
    if (ImGui::SliderFloat("Audio Bitrate (kbps)", &audioBitrateKbps, 64.0f, 320.0f, "%.0f"))
    {
        m_uiManager->triggerStreamingAudioBitrateChange(static_cast<uint32_t>(audioBitrateKbps));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("128 kbps is transparent for AAC; 256 kbps is\n"
                          "the safe default. Opus reaches the same\n"
                          "quality around 96 kbps.");
    }
}

void UIConfigurationStreaming::renderAdvancedBufferSettings()
{
    ui_section_header("Buffer (Advanced)",
                      "Lower-level synchronizer tunables. Defaults work "
                      "for nearly every setup — only touch if you know "
                      "exactly which timing issue you're chasing.");

    // Max Video Buffer Size
    int maxVideoBuffer = static_cast<int>(m_uiManager->getStreamingMaxVideoBufferSize());
    if (ImGui::SliderInt("Max Frames in Buffer", &maxVideoBuffer, 1, 50))
    {
        m_uiManager->triggerStreamingMaxVideoBufferSizeChange(static_cast<size_t>(maxVideoBuffer));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Max video frames in the buffer.\n"
                          "1-50 frames: valid range\n"
                          "Default: 10 frames\n"
                          "Higher values = more memory, less risk of dropped frames");
    }

    // Max Audio Buffer Size
    int maxAudioBuffer = static_cast<int>(m_uiManager->getStreamingMaxAudioBufferSize());
    if (ImGui::SliderInt("Max Chunks in Buffer", &maxAudioBuffer, 5, 100))
    {
        m_uiManager->triggerStreamingMaxAudioBufferSizeChange(static_cast<size_t>(maxAudioBuffer));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Max audio chunks in the buffer.\n"
                          "5-100 chunks: valid range\n"
                          "Default: 20 chunks\n"
                          "Higher values = more memory, better sync");
    }

    // Max Buffer Time
    int maxBufferTime = static_cast<int>(m_uiManager->getStreamingMaxBufferTimeSeconds());
    if (ImGui::SliderInt("Max Buffer Time (seconds)", &maxBufferTime, 1, 30))
    {
        m_uiManager->triggerStreamingMaxBufferTimeSecondsChange(static_cast<int64_t>(maxBufferTime));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Max buffer time in seconds.\n"
                          "1-30 seconds: valid range\n"
                          "Default: 5 seconds\n"
                          "Controls how much video/audio can be queued before processing");
    }

    // AVIO Buffer Size
    int avioBuffer = static_cast<int>(m_uiManager->getStreamingAVIOBufferSize() / 1024); // Converter para KB
    if (ImGui::SliderInt("AVIO Buffer (KB)", &avioBuffer, 64, 1024))
    {
        m_uiManager->triggerStreamingAVIOBufferSizeChange(static_cast<size_t>(avioBuffer * 1024));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("FFmpeg AVIO buffer size in KB.\n"
                          "64-1024 KB: valid range\n"
                          "Default: 256 KB\n"
                          "FFmpeg internal buffer for streaming I/O");
    }
}

void UIConfigurationStreaming::renderStartStopButton()
{
    // Botão Start/Stop
    bool active = m_uiManager->getStreamingActive();
    bool processing = m_uiManager->isStreamingProcessing();
    int64_t cooldownMs = m_uiManager->getStreamingCooldownRemainingMs();

    // Desabilitar botão se estiver processando (start/stop em andamento)
    if (processing)
    {
        ImGui::BeginDisabled();
        if (active)
        {
            ImGui::Button("Parando...", ImVec2(-1, 0));
        }
        else
        {
            ImGui::Button("Iniciando...", ImVec2(-1, 0));
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Aguarde o processo terminar");
        }
    }
    else if (active)
    {
        if (ImGui::Button("Stop Streaming", ImVec2(-1, 0)))
        {
            m_uiManager->setStreamingProcessing(true); // Marcar como processando
            m_uiManager->triggerStreamingStartStop(false);
        }
    }
    else
    {
        // Desabilitar botão se estiver em cooldown
        if (cooldownMs > 0)
        {
            ImGui::BeginDisabled();
            float cooldownSeconds = cooldownMs / 1000.0f;
            std::string label = "Aguardando (" + std::to_string(static_cast<int>(cooldownSeconds)) + "s)";
            ImGui::Button(label.c_str(), ImVec2(-1, 0));
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Aguarde o cooldown terminar antes de iniciar o streaming novamente");
            }
        }
        else
        {
            if (ImGui::Button("Start Streaming", ImVec2(-1, 0)))
            {
                m_uiManager->setStreamingProcessing(true); // Marcar como processando
                m_uiManager->triggerStreamingStartStop(true);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────
// Public directory publish (#49 Phase 2)
//
// Opt-in section that lets the host announce its stream in the
// directory service. The actual networking lives in Application's
// DirectoryClient instance; this UI is purely a user-facing surface
// that toggles enabled-state and edits the metadata fields, then
// reads back a status string Application writes after each
// register/heartbeat tick.
// ─────────────────────────────────────────────────────────────────────
void UIConfigurationStreaming::renderDirectoryPublish()
{
    ui_section_header("Public directory",
                      "Optionally list this stream in a public directory "
                      "so other RetroCapture clients can find it without "
                      "you sharing the URL out of band.");

    // ── Privacy modal: shown the first time the user flips the toggle
    // on. Once accepted, the ack flag sticks and we never show it
    // again. The actual OpenPopup() call has to happen *inside* the
    // same window pass as the BeginPopupModal that follows.
    if (m_dirShowPrivacyModal)
    {
        ImGui::OpenPopup("Publish to public directory");
        m_dirShowPrivacyModal = false;
    }
    if (ImGui::BeginPopupModal("Publish to public directory", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped(
            "Publishing exposes your stream's connection information "
            "(URL and, in 'Direct' mode, your public IP) to anyone "
            "browsing the directory.");
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Consider using a reverse tunnel (Cloudflare Tunnel, "
            "Tailscale Funnel, your own VPS) and selecting 'Custom' "
            "endpoint mode if you'd rather not expose your home IP.");
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Accept and publish", ImVec2(180, 0)))
        {
            m_uiManager->setDirectoryPrivacyAcked(true);
            m_uiManager->setDirectoryPublishEnabled(true);
            m_uiManager->saveConfig();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            m_uiManager->setDirectoryPublishEnabled(false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ── Editable fields, always visible. The user fills these in
    // first, then flips the toggle below. saveConfig() persists each
    // edit so the settings survive across runs.
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", m_uiManager->getDirectoryStreamName().c_str());
        if (ImGui::InputText("Stream name *", buf, sizeof(buf)))
        {
            m_uiManager->setDirectoryStreamName(buf);
            m_uiManager->saveConfig();
        }
    }
    // #84 — Nickname now comes from the chat Profile (single source
    // of truth for the user's display name across stream + chat).
    // Show a read-only label and a button that pops the Profile
    // window via UIManager's one-shot request flag.
    {
        const std::string &nick = m_uiManager->getChatNickname();
        if (nick.empty())
        {
            ImGui::TextDisabled("Nickname: ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.30f, 1.0f),
                               "(not set)");
        }
        else
        {
            ImGui::TextDisabled("Nickname:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.95f, 0.90f, 1.0f),
                               "%s", nick.c_str());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Configure Profile"))
        {
            m_uiManager->requestOpenChatProfile();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Your display name is taken from the chat Profile\n"
                "(name + nickname + persistent rc_<id>). Click to\n"
                "edit it; the same nickname is used in the directory\n"
                "listing AND in chat.");
        }
    }
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", m_uiManager->getDirectoryPassword().c_str());
        if (ImGui::InputText("Password (optional)", buf, sizeof(buf), ImGuiInputTextFlags_Password))
        {
            m_uiManager->setDirectoryPassword(buf);
            m_uiManager->saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "If set, clients must enter this password to connect.\n"
                "The directory only stores the flag that a password is\n"
                "required — never the password itself.");
        }
    }

    // #84 — Chat-with-stream toggle + persistent room slug. When the
    // toggle is on, Application provisions a standalone chat room
    // (named by the slug below) on the first stream start and binds
    // viewers to it via /meta. The slug is editable until a room
    // exists for it — after that, changing it would orphan the old
    // room and the user would lose their is_owner grant, so we lock
    // the field once owned_rooms.json has a matching entry.
    {
        bool chatOn = m_uiManager->getStreamChatEnabled();
        if (ImGui::Checkbox("Open chat alongside this stream", &chatOn))
        {
            m_uiManager->setStreamChatEnabled(chatOn);
            // #84 — When the user turns chat on but hasn't named
            // their stream yet, seed a default like "Stream of
            // <nick>" so the directory entry has something readable.
            // The user can still edit the field afterwards. We only
            // fill when nickname is non-empty; without a nickname
            // we'd write "Stream of " which is worse than nothing.
            if (chatOn &&
                m_uiManager->getDirectoryStreamName().empty() &&
                !m_uiManager->getChatNickname().empty())
            {
                m_uiManager->setDirectoryStreamName(
                    std::string("Stream of ") + m_uiManager->getChatNickname());
            }
            m_uiManager->saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "When on, viewers' chat panel auto-binds to your\n"
                "persistent room (slug below) as soon as they tune in.\n"
                "When off, the stream goes live without a chat panel —\n"
                "viewers can still join any room manually.");
        }
        if (chatOn)
        {
            char slugBuf[64];
            std::snprintf(slugBuf, sizeof(slugBuf), "%s",
                          m_uiManager->getStreamRoomSlug().c_str());
            OwnedRoom owned;
            const bool alreadyProvisioned =
                !m_uiManager->getStreamRoomSlug().empty() &&
                ownedrooms::findBySlug(m_uiManager->getStreamRoomSlug(),
                                       owned);
            ImGui::BeginDisabled(alreadyProvisioned);
            if (ImGui::InputText("Room slug *", slugBuf, sizeof(slugBuf)))
            {
                // Normalise: lowercase + strip whitespace. The chat
                // service has its own slug validator that'll reject
                // anything outside [a-z0-9-_]; let the user keep
                // typing freely and trim on save.
                std::string s = slugBuf;
                while (!s.empty() && std::isspace((unsigned char)s.back()))
                    s.pop_back();
                for (auto &c : s)
                    c = static_cast<char>(std::tolower((unsigned char)c));
                m_uiManager->setStreamRoomSlug(s);
                m_uiManager->saveConfig();
            }
            ImGui::EndDisabled();
            if (alreadyProvisioned)
            {
                ImGui::TextDisabled(
                    "Locked: the room has been provisioned. Delete it\n"
                    "from Chat → Rooms → Owned to pick a new slug.");
            }
            else if (m_uiManager->getStreamRoomSlug().empty())
            {
                ImGui::TextDisabled(
                    "Pick a public name for your chat room (e.g.\n"
                    "your nickname). The room is created on your\n"
                    "next stream start and you own it forever.");
            }
            else
            {
                ImGui::TextDisabled(
                    "The room will be created on your next stream\n"
                    "start.");
            }
        }
    }

    // Endpoint-mode dropdown.
    //
    // Cloudflare Tunnel only appears on platforms where cloudflared
    // has an upstream release (linux/amd64, linux/arm64, win/amd64
    // today — see CloudflaredDownloader::isPlatformSupported). On
    // ARM32 and other unsupported targets we drop the entry from the
    // list entirely and fall back to Direct / Custom URL.
    {
        const bool cfSupported = CloudflaredDownloader::isPlatformSupported();
        std::vector<const char *> modes  = { "Direct (port-forwarded)" };
        std::vector<const char *> keys   = { "direct" };
        if (cfSupported)
        {
            modes.push_back("Cloudflare Tunnel");
            keys.push_back("tunnel-cloudflare");
        }
        modes.push_back("Custom URL");
        keys.push_back("custom");

        // If the stored mode is now unavailable (e.g. saved config from
        // an x86_64 host and the user opened on a Pi 3), fall back to
        // Direct so the dropdown isn't stuck on a missing entry.
        std::string stored = m_uiManager->getDirectoryEndpointMode();
        int current = 0;
        bool storedFound = false;
        for (size_t i = 0; i < keys.size(); ++i)
        {
            if (stored == keys[i]) { current = static_cast<int>(i); storedFound = true; break; }
        }
        if (!storedFound)
        {
            m_uiManager->setDirectoryEndpointMode("direct");
            m_uiManager->saveConfig();
            current = 0;
        }

        if (ImGui::Combo("Endpoint mode", &current,
                         modes.data(), static_cast<int>(modes.size())))
        {
            m_uiManager->setDirectoryEndpointMode(keys[current]);
            m_uiManager->saveConfig();
        }
        if (!cfSupported && ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Cloudflare Tunnel is hidden because there's "
                              "no upstream cloudflared binary for this "
                              "architecture.");
        }
    }

    const std::string endpointMode = m_uiManager->getDirectoryEndpointMode();
    if (endpointMode == "custom")
    {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", m_uiManager->getDirectoryCustomEndpoint().c_str());
        if (ImGui::InputText("Custom endpoint URL", buf, sizeof(buf)))
        {
            m_uiManager->setDirectoryCustomEndpoint(buf);
            m_uiManager->saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Paste the public URL you've already set up\n"
                              "(FRP, Tailscale Funnel, ngrok, your own domain…).");
        }
        // Inline validation — shown only when the user has typed
        // *something*, so an empty field doesn't shout at them
        // before they've had a chance to fill it in. The 'fill it in'
        // case is handled by the publish-toggle gate below.
        {
            const std::string current = m_uiManager->getDirectoryCustomEndpoint();
            std::string err;
            if (!current.empty() && !validateCustomEndpoint(current, err))
            {
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f),
                                   "Invalid URL: %s", err.c_str());
            }
        }
    }
    else if (endpointMode == "tunnel-cloudflare")
    {
        // Tunnel sub-mode radio. Quick is the zero-friction default
        // (random trycloudflare.com URL per run, no account). Named
        // requires a Cloudflare account + a domain on Cloudflare but
        // gives a persistent shareable URL.
        std::string tunnelMode = m_uiManager->getDirectoryTunnelMode();
        if (tunnelMode != "quick" && tunnelMode != "named")
        {
            tunnelMode = "quick";
            m_uiManager->setDirectoryTunnelMode(tunnelMode);
            m_uiManager->saveConfig();
        }
        ImGui::Text("Tunnel type:");
        bool isQuick = (tunnelMode == "quick");
        if (ImGui::RadioButton("Quick (random URL each run)", isQuick))
        {
            if (!isQuick)
            {
                m_uiManager->setDirectoryTunnelMode("quick");
                m_uiManager->saveConfig();
                tunnelMode = "quick";
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("No Cloudflare account needed. The URL "
                              "changes every time you publish.");
        }
        ImGui::SameLine();
        bool isNamed = (tunnelMode == "named");
        if (ImGui::RadioButton("Named (persistent URL)", isNamed))
        {
            if (!isNamed)
            {
                m_uiManager->setDirectoryTunnelMode("named");
                m_uiManager->saveConfig();
                tunnelMode = "named";
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Requires a Cloudflare account and a "
                              "domain registered/proxied on Cloudflare. "
                              "Gives you a stable URL you can share.");
        }
        ImGui::Spacing();

        if (tunnelMode == "named")
        {
            ImGui::TextDisabled(
                "RetroCapture will run `cloudflared tunnel run` against your\n"
                "named tunnel. The directory entry advertises your hostname\n"
                "instead of a random trycloudflare.com URL.");
        }
        else
        {
            ImGui::TextDisabled(
                "RetroCapture will run `cloudflared` to expose your stream via\n"
                "a Cloudflare Quick Tunnel. No Cloudflare account needed; the\n"
                "URL is randomized each run. Public IP is never exposed.");
        }
        ImGui::Spacing();
        renderCloudflaredDownload();
        if (tunnelMode == "named")
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            renderCloudflaredNamedTunnelSetup();
        }
    }
    else
    {
        ImGui::TextDisabled(
            "Direct mode uses your local streaming endpoint as advertised.\n"
            "Requires your stream port to be reachable from the public internet\n"
            "(port-forwarding) — won't work behind CGNAT.");
    }

    // Advanced: directory URL override + chat URL + TLS dev toggle.
    if (ImGui::TreeNode("Advanced"))
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", m_uiManager->getDirectoryUrl().c_str());
        if (ImGui::InputText("Directory URL", buf, sizeof(buf)))
        {
            m_uiManager->setDirectoryUrl(buf);
            m_uiManager->saveConfig();
        }
        // #84 — Chat service URL. Default: production.
        // Accepts any of https:// / http:// / wss:// / ws:// — the
        // client normalizes internally between the REST resolve /
        // history endpoints (http/https) and the WS upgrade (ws/wss).
        char chatBuf[256];
        std::snprintf(chatBuf, sizeof(chatBuf), "%s",
                      m_uiManager->getChatBaseUrl().c_str());
        if (ImGui::InputText("Chat URL", chatBuf, sizeof(chatBuf)))
        {
            m_uiManager->setChatBaseUrl(chatBuf);
            m_uiManager->saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Chat service base URL. Accepts https:// / http:// /\n"
                "wss:// / ws:// — the client picks the right scheme\n"
                "for each endpoint internally.\n"
                "Default: https://chat.retrocapture.com\n"
                "Local dev: http://localhost:8082");
        }
        // Dev escape hatch — only meaningful for https:// URLs.
        // Off by default. The label spells out "self-signed" because
        // that's the only legitimate use; flipping it for the public
        // directory hides cert-rotation problems.
        bool skip = m_uiManager->getDirectoryInsecureSkipVerify();
        if (ImGui::Checkbox("Allow self-signed TLS cert (dev only)", &skip))
        {
            m_uiManager->setDirectoryInsecureSkipVerify(skip);
            m_uiManager->saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Skip TLS peer-certificate verification when talking to\n"
                "the directory service. Use ONLY against a self-signed\n"
                "dev host — never the public directory.");
        }
        ImGui::TreePop();
    }

    ImGui::Spacing();

    // ── Master toggle. Three things have to be true to flip it on:
    //   - the user filled in a non-empty stream name
    //   - in 'custom' mode, the user filled in a non-empty endpoint
    //   - the user has accepted the privacy warning (modal handles
    //     this; the toggle stays off until accept fires)
    //
    // We compute a one-line reason string that explains why the
    // toggle is disabled, so the user doesn't have to guess.
    const std::string trimmedName     = m_uiManager->getDirectoryStreamName();
    const std::string trimmedCustom   = m_uiManager->getDirectoryCustomEndpoint();
    const std::string mode            = m_uiManager->getDirectoryEndpointMode();
    const bool        currentlyOn     = m_uiManager->getDirectoryPublishEnabled();
    std::string       blockedReason;
    if (trimmedName.empty())
    {
        blockedReason = "Fill in 'Stream name' to enable publishing.";
    }
    else if (mode == "custom" && trimmedCustom.empty())
    {
        blockedReason = "Fill in the custom endpoint URL to enable publishing.";
    }
    else if (mode == "custom")
    {
        std::string customErr;
        if (!validateCustomEndpoint(trimmedCustom, customErr))
        {
            blockedReason = "Custom endpoint URL is invalid (" + customErr +
                            "). Fix it to enable publishing.";
        }
    }
    const bool canToggle = blockedReason.empty();

    // The checkbox is always rendered; we just disable it when the
    // prerequisites aren't met. A user already mid-publish sees the
    // checkbox on; clearing the name field doesn't kick them out
    // because the running DirectoryClient holds its own Config copy.
    if (!canToggle && !currentlyOn)
    {
        ImGui::BeginDisabled();
    }
    bool enabled = currentlyOn;
    if (ImGui::Checkbox("Publish this stream to the public directory", &enabled))
    {
        if (enabled)
        {
            // #84 — Safety-net auto-fill: the chat-toggle handler
            // already seeds "Stream of <nick>" but only if both the
            // toggle AND a nickname were set at that moment. If the
            // user did it the other way around (configured profile
            // AFTER ticking chat, or just left the field empty), do
            // the fill one more time here so they don't get rejected
            // by the empty-name validation.
            if (m_uiManager->getStreamChatEnabled() &&
                m_uiManager->getDirectoryStreamName().empty() &&
                !m_uiManager->getChatNickname().empty())
            {
                m_uiManager->setDirectoryStreamName(
                    std::string("Stream of ") +
                    m_uiManager->getChatNickname());
            }
            if (!m_uiManager->getDirectoryPrivacyAcked())
            {
                // Need consent first. Modal flips the toggle on once
                // accepted, or leaves it off on cancel.
                m_uiManager->setDirectoryPublishEnabled(false);
                m_dirShowPrivacyModal = true;
            }
            else
            {
                m_uiManager->setDirectoryPublishEnabled(true);
                m_uiManager->saveConfig();
            }
        }
        else
        {
            m_uiManager->setDirectoryPublishEnabled(false);
            m_uiManager->saveConfig();
        }
    }
    if (!canToggle && !currentlyOn)
    {
        ImGui::EndDisabled();
    }

    if (!canToggle && !currentlyOn)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", blockedReason.c_str());
    }

    // Status surface. Application writes a one-line string after every
    // register / heartbeat cycle; we just display it.
    ImGui::Spacing();
    ImGui::Text("Status:");
    ImGui::SameLine();
    const std::string &status = m_uiManager->getDirectoryStatusText();
    if (status.rfind("Active", 0) == 0)
    {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", status.c_str());
    }
    else if (status.rfind("Error", 0) == 0)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s", status.c_str());
    }
    else
    {
        ImGui::TextDisabled("%s", status.c_str());
    }

    // Cloudflare Quick Tunnel URLs come from a fresh wildcard DNS entry
    // that Cloudflare's resolver needs a moment to publish globally.
    // Empirically anywhere from a few seconds to a couple of minutes,
    // depending on which resolver the client side hits. Surface this
    // up front so a user who clicks Publish and immediately tries to
    // connect from another machine doesn't conclude the tunnel is
    // broken when really it just hasn't propagated yet. Named tunnels
    // skip this — those use the user's own zone where TTLs are
    // typically tight.
    {
        const bool isCfTunnel = (m_uiManager->getDirectoryEndpointMode() == "tunnel-cloudflare");
        const bool isQuick    = (m_uiManager->getDirectoryTunnelMode() == "quick");
        if (isCfTunnel && isQuick && status.rfind("Active", 0) == 0)
        {
            ImGui::TextDisabled(
                "Note: the trycloudflare.com URL can take up to ~2 minutes\n"
                "to resolve from other networks (DNS propagation). Browsers\n"
                "and clients trying to connect immediately may see 'host not\n"
                "found' — that clears on its own once DNS catches up.");
        }
    }

    // Per-session telemetry (#49 Phase 5). Hidden behind a tree node
    // so the publish section stays compact for normal users; opens
    // on demand when something looks off and the user wants to dig.
    if (ImGui::TreeNode("Telemetry"))
    {
        const int64_t since = m_uiManager->getDirectorySecondsSinceLastHeartbeat();
        ImGui::Text("register   ok=%llu  fail=%llu",
                    (unsigned long long)m_uiManager->getDirectoryRegisterOk(),
                    (unsigned long long)m_uiManager->getDirectoryRegisterFail());
        ImGui::Text("heartbeat  ok=%llu  fail=%llu",
                    (unsigned long long)m_uiManager->getDirectoryHeartbeatOk(),
                    (unsigned long long)m_uiManager->getDirectoryHeartbeatFail());
        ImGui::Text("patch      ok=%llu  fail=%llu",
                    (unsigned long long)m_uiManager->getDirectoryPatchOk(),
                    (unsigned long long)m_uiManager->getDirectoryPatchFail());
        if (since < 0)
        {
            ImGui::TextDisabled("last successful heartbeat: never");
        }
        else
        {
            ImGui::Text("last successful heartbeat: %llds ago", (long long)since);
        }
        ImGui::TreePop();
    }
}

// ─────────────────────────────────────────────────────────────────────
// Cloudflared auto-download UI (#53 / Phase 2.5b)
//
// Rendered inline inside the Cloudflare Tunnel branch of the endpoint
// dropdown rather than as a modal, because:
//   * the user is already looking at this row of the config — popping
//     a modal on top would just hide the context for the action;
//   * the worker thread fires progress updates several times a second
//     and an inline progress bar is the natural place to surface them;
//   * Cancel is a no-op (no abort hook on the worker yet) so a modal
//     with an active Cancel button would be misleading.
//
// State is read out of m_cfProgress under m_cfMu — the worker thread
// writes there from beginDownloadAsync's callback.
// ─────────────────────────────────────────────────────────────────────
void UIConfigurationStreaming::renderCloudflaredDownload()
{
    using Stage = CloudflaredDownloader::Stage;

    // Snapshot under the lock so we don't tear a half-written
    // std::string between the worker writing and the UI reading.
    CloudflaredDownloader::Progress snap;
    {
        std::lock_guard<std::mutex> lock(m_cfMu);
        snap = m_cfProgress;
    }

    const bool resolved = !CloudflaredDownloader::resolveBinaryPath().empty();
    const bool cached   = CloudflaredDownloader::isCached();
    const bool busy     = (snap.stage == Stage::Connecting ||
                           snap.stage == Stage::Downloading ||
                           snap.stage == Stage::Verifying ||
                           snap.stage == Stage::Installing) &&
                          m_cfStartedThisRun;

    // Resolved & not currently downloading → all good, just say so.
    if (resolved && !busy)
    {
        ImGui::TextColored(ImVec4(0.40f, 0.80f, 0.40f, 1.0f),
                           "cloudflared ready.");
        ImGui::SameLine();
        if (cached)
        {
            ImGui::TextDisabled("(downloaded copy, version %s)",
                                CloudflaredDownloader::pinnedVersion().c_str());
        }
        else
        {
            ImGui::TextDisabled("(found on PATH)");
        }
        return;
    }

    // Busy path: progress bar + stage line, no buttons.
    if (busy)
    {
        const char *stageLabel = "Working...";
        switch (snap.stage)
        {
            case Stage::Connecting:  stageLabel = "Connecting to github.com..."; break;
            case Stage::Downloading: stageLabel = "Downloading cloudflared..."; break;
            case Stage::Verifying:   stageLabel = "Verifying SHA256..."; break;
            case Stage::Installing:  stageLabel = "Installing..."; break;
            default: break;
        }
        ImGui::TextUnformatted(stageLabel);
        if (snap.stage == Stage::Downloading)
        {
            char overlay[64];
            if (snap.bytesTotal > 0)
            {
                std::snprintf(overlay, sizeof(overlay), "%.1f / %.1f MB",
                              static_cast<double>(snap.bytesDone)  / (1024.0 * 1024.0),
                              static_cast<double>(snap.bytesTotal) / (1024.0 * 1024.0));
            }
            else
            {
                std::snprintf(overlay, sizeof(overlay), "%.1f MB",
                              static_cast<double>(snap.bytesDone) / (1024.0 * 1024.0));
            }
            ImGui::ProgressBar(snap.progress01, ImVec2(-1.0f, 0.0f), overlay);
        }
        else
        {
            // Indeterminate stages — show a full bar with the stage
            // label so the user has visual feedback that something is
            // happening.
            ImGui::ProgressBar(1.0f, ImVec2(-1.0f, 0.0f), stageLabel);
        }
        return;
    }

    // Failed since last attempt: show the error, allow retry.
    if (snap.stage == Stage::Failed && m_cfStartedThisRun)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f),
                           "Download failed:");
        ImGui::TextWrapped("%s", snap.error.empty()
                                     ? "(unknown error)"
                                     : snap.error.c_str());
        ImGui::Spacing();
    }

    // Idle / Failed state: offer the download. Disable the button on
    // unsupported platforms (defence in depth — the ARM32 path also
    // hides the whole Cloudflare Tunnel dropdown entry).
    const bool supported = CloudflaredDownloader::isPlatformSupported();
    ImGui::TextWrapped(
        "cloudflared isn't installed yet. RetroCapture can download it "
        "(~%s MB, %s) from the official Cloudflare release on GitHub. "
        "The file is verified with a pinned SHA256 before being used.",
        // Rough binary size for the UI — exact value is in the .cpp.
        "40",
        CloudflaredDownloader::pinnedVersion().c_str());
    ImGui::Spacing();
    ImGui::BeginDisabled(!supported);
    if (ImGui::Button("Download cloudflared", ImVec2(220, 0)))
    {
        m_cfStartedThisRun = true;
        {
            std::lock_guard<std::mutex> lock(m_cfMu);
            m_cfProgress = {};
            m_cfProgress.stage = Stage::Connecting;
        }
        CloudflaredDownloader::beginDownloadAsync(
            [this](const CloudflaredDownloader::Progress &p) {
                std::lock_guard<std::mutex> lock(m_cfMu);
                m_cfProgress = p;
            });
    }
    ImGui::EndDisabled();
    if (!supported && ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("No upstream cloudflared binary for this architecture.");
    }
}

// ─────────────────────────────────────────────────────────────────────
// Named Cloudflare Tunnel setup UI (#60 / Phase 2.5c)
//
// Renders below renderCloudflaredDownload() when the user has picked
// Named as the tunnel sub-mode. Three concerns layered top to bottom:
//
//   1. Authentication. Spawn `cloudflared tunnel login`, surface the
//      OAuth URL it prints so a headless / SSH user can copy-paste
//      it. Stage updates fire from the worker thread; we sample
//      under m_cfNamedMu.
//
//   2. Tunnel selection. Once cert.pem is on disk, list the user's
//      existing tunnels and offer a "Create new..." modal. Selection
//      writes into UIManager (persisted), so the next run picks the
//      same one.
//
//   3. Hostname + DNS route. User types a hostname under a domain
//      they already own on Cloudflare, hits "Apply DNS route", we
//      shell out to `cloudflared tunnel route dns`. Result text
//      sticks around until they retry.
//
// Heavy operations (login, list, create, route) all run on detached
// threads so the UI stays responsive — listTunnels can take ~10 s,
// login can take minutes waiting for the user to OAuth.
// ─────────────────────────────────────────────────────────────────────
void UIConfigurationStreaming::renderCloudflaredNamedTunnelSetup()
{
    using LoginStage = CloudflaredAccount::LoginProgress::Stage;

    // We need a usable binary before any of this works. The download
    // section above already handles the unhappy path; here we just
    // gate the named-tunnel UI on it so the buttons aren't dead
    // links.
    if (CloudflaredDownloader::resolveBinaryPath().empty())
    {
        ImGui::TextDisabled("cloudflared isn't available yet — finish the "
                            "download above before setting up a named tunnel.");
        return;
    }

    // Scope all the ImGui IDs in this section under "namedTunnel" so
    // generic button labels ("Refresh", "Cancel", "Create") don't
    // collide with identically-named buttons rendered elsewhere in
    // the same window (the streaming profiles block has its own
    // "Refresh"). Without this ImGui pops up the "2 visible items
    // with conflicting ID" debug overlay.
    ImGui::PushID("namedTunnel");

    ImGui::Text("Named Cloudflare Tunnel");
    ImGui::Spacing();

    // ── 1. Authentication ──────────────────────────────────────
    const bool hasCreds = CloudflaredAccount::hasCredentials();

    CloudflaredAccount::LoginProgress loginSnap;
    bool loginActive;
    {
        std::lock_guard<std::mutex> lock(m_cfNamedMu);
        loginSnap   = m_loginProgress;
        loginActive = m_loginStartedThisRun &&
                      (loginSnap.stage == LoginStage::Starting ||
                       loginSnap.stage == LoginStage::AwaitingAuth);
    }

    if (!hasCreds || loginActive)
    {
        if (loginActive)
        {
            if (loginSnap.stage == LoginStage::AwaitingAuth && !loginSnap.oauthUrl.empty())
            {
                ImGui::TextWrapped(
                    "Waiting for Cloudflare authentication. If a browser didn't "
                    "open automatically, copy this URL into one:");
                ImGui::Spacing();
                // Show the URL in a wide read-only input so the user can
                // select-all + copy on any platform.
                char urlBuf[1024];
                std::snprintf(urlBuf, sizeof(urlBuf), "%s", loginSnap.oauthUrl.c_str());
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##oauthUrl", urlBuf, sizeof(urlBuf),
                                 ImGuiInputTextFlags_ReadOnly);
            }
            else
            {
                ImGui::TextDisabled("Starting Cloudflare sign-in...");
            }
            ImGui::Spacing();
            if (ImGui::Button("Cancel sign-in", ImVec2(160, 0)))
            {
                CloudflaredAccount::cancelLogin();
            }
        }
        else
        {
            ImGui::TextWrapped(
                "You'll be redirected to Cloudflare to authenticate. After "
                "logging in, Cloudflare writes a credentials file locally; "
                "RetroCapture uses it to talk to your account.");
            ImGui::Spacing();
            if (ImGui::Button("Sign in to Cloudflare...", ImVec2(220, 0)))
            {
                {
                    std::lock_guard<std::mutex> lock(m_cfNamedMu);
                    m_loginProgress       = {};
                    m_loginProgress.stage = LoginStage::Starting;
                    m_loginStartedThisRun = true;
                }
                CloudflaredAccount::beginLoginAsync(
                    [this](const CloudflaredAccount::LoginProgress &p) {
                        std::lock_guard<std::mutex> lock(m_cfNamedMu);
                        m_loginProgress = p;
                    });
            }
            if (loginSnap.stage == LoginStage::Failed && m_loginStartedThisRun)
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f),
                                   "Sign-in failed:");
                ImGui::TextWrapped("%s", loginSnap.error.c_str());
            }
            else if (loginSnap.stage == LoginStage::Cancelled && m_loginStartedThisRun)
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
                                   "Sign-in cancelled. Click again to retry.");
            }
        }
        return;
    }

    ImGui::TextColored(ImVec4(0.40f, 0.80f, 0.40f, 1.0f),
                       "Signed in to Cloudflare.");
    ImGui::SameLine();
    ImGui::TextDisabled("(credentials in ~/.cloudflared/)");
    ImGui::Spacing();

    // ── 2. Tunnel selection ─────────────────────────────────────
    // Cached list state under lock; refresh button triggers an async
    // refetch. First render of this section also kicks off a refresh
    // if we don't have one cached yet.
    std::vector<CloudflaredAccount::TunnelInfo> tunnels;
    std::string tunnelsError;
    bool        loaded;
    {
        std::lock_guard<std::mutex> lock(m_cfNamedMu);
        tunnels      = m_namedTunnels;
        tunnelsError = m_namedTunnelsError;
        loaded       = m_namedTunnelsLoaded;
    }

    auto kickOffRefresh = [this]() {
        if (m_namedTunnelsRefreshing.exchange(true)) return;
        std::thread([this]() {
            std::string err;
            auto fresh = CloudflaredAccount::listTunnels(err);
            std::lock_guard<std::mutex> lock(m_cfNamedMu);
            m_namedTunnels       = std::move(fresh);
            m_namedTunnelsError  = std::move(err);
            m_namedTunnelsLoaded = true;
            m_namedTunnelsRefreshing.store(false);
        }).detach();
    };
    if (!loaded && !m_namedTunnelsRefreshing.load())
    {
        kickOffRefresh();
    }

    const std::string currentTunnelId = m_uiManager->getDirectoryNamedTunnelId();
    std::string currentLabel = currentTunnelId.empty()
        ? "(no tunnel selected)"
        : currentTunnelId; // overridden below if we find a name match
    for (const auto &t : tunnels)
    {
        if (t.id == currentTunnelId)
        {
            currentLabel = t.name + " (" + t.id.substr(0, 8) + "…)";
            break;
        }
    }
    ImGui::Text("Tunnel:");
    ImGui::SetNextItemWidth(-120);
    if (ImGui::BeginCombo("##namedTunnel", currentLabel.c_str()))
    {
        for (const auto &t : tunnels)
        {
            bool selected = (t.id == currentTunnelId);
            std::string lbl = t.name + " (" + t.id.substr(0, 8) + "…)";
            if (ImGui::Selectable(lbl.c_str(), selected))
            {
                m_uiManager->setDirectoryNamedTunnelId(t.id);
                m_uiManager->saveConfig();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::Separator();
        if (ImGui::Selectable("Create new tunnel..."))
        {
            std::memset(m_newTunnelName, 0, sizeof(m_newTunnelName));
            m_createTunnelError.clear();
            m_showCreateTunnelModal = true;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (m_namedTunnelsRefreshing.load())
    {
        ImGui::BeginDisabled(); ImGui::Button("Refreshing...", ImVec2(110, 0)); ImGui::EndDisabled();
    }
    else if (ImGui::Button("Refresh", ImVec2(110, 0)))
    {
        kickOffRefresh();
    }
    if (!tunnelsError.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f),
                           "Tunnel list error: %s", tunnelsError.c_str());
    }

    // Create-new modal
    if (m_showCreateTunnelModal) ImGui::OpenPopup("Create Tunnel");
    if (ImGui::BeginPopupModal("Create Tunnel", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Tunnel name (will be visible in your Cloudflare dashboard):");
        ImGui::SetNextItemWidth(280);
        ImGui::InputText("##newTunnelName", m_newTunnelName, sizeof(m_newTunnelName));
        if (!m_createTunnelError.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "%s",
                               m_createTunnelError.c_str());
        }
        const bool busy = m_createTunnelInFlight.load();
        ImGui::BeginDisabled(busy || std::strlen(m_newTunnelName) == 0);
        if (ImGui::Button(busy ? "Creating..." : "Create", ImVec2(120, 0)))
        {
            m_createTunnelInFlight.store(true);
            m_createTunnelError.clear();
            std::string name = m_newTunnelName;
            std::thread([this, name]() {
                std::string err;
                std::string id = CloudflaredAccount::createTunnel(name, err);
                if (!id.empty())
                {
                    m_uiManager->setDirectoryNamedTunnelId(id);
                    m_uiManager->saveConfig();
                    // Re-fetch so the new entry shows up immediately.
                    std::string listErr;
                    auto fresh = CloudflaredAccount::listTunnels(listErr);
                    {
                        std::lock_guard<std::mutex> lock(m_cfNamedMu);
                        m_namedTunnels      = std::move(fresh);
                        m_namedTunnelsError = std::move(listErr);
                        m_namedTunnelsLoaded = true;
                        m_showCreateTunnelModal = false;
                    }
                }
                else
                {
                    std::lock_guard<std::mutex> lock(m_cfNamedMu);
                    m_createTunnelError = err.empty()
                        ? "Unknown error creating tunnel"
                        : err;
                }
                m_createTunnelInFlight.store(false);
            }).detach();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            m_showCreateTunnelModal = false;
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }

    ImGui::Spacing();

    // ── 3. Hostname + DNS route ─────────────────────────────────
    ImGui::Text("Hostname:");
    char hostBuf[256];
    std::snprintf(hostBuf, sizeof(hostBuf), "%s",
                  m_uiManager->getDirectoryNamedTunnelHostname().c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##namedHost", hostBuf, sizeof(hostBuf)))
    {
        m_uiManager->setDirectoryNamedTunnelHostname(hostBuf);
        m_uiManager->saveConfig();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("e.g. stream.example.com — must be on a domain you\n"
                          "already control on Cloudflare. RetroCapture asks\n"
                          "cloudflared to register the DNS record under your\n"
                          "zone; if the apex isn't on Cloudflare the route\n"
                          "command will fail with a clear message below.");
    }

    const bool routeBusy = m_routeInFlight.load();
    ImGui::BeginDisabled(routeBusy ||
                         currentTunnelId.empty() ||
                         std::strlen(hostBuf) == 0);
    if (ImGui::Button(routeBusy ? "Applying..." : "Apply DNS route",
                      ImVec2(180, 0)))
    {
        m_routeInFlight.store(true);
        std::string id  = currentTunnelId;
        std::string h   = hostBuf;
        std::thread([this, id, h]() {
            std::string err;
            bool ok = CloudflaredAccount::routeDns(id, h, err);
            std::lock_guard<std::mutex> lock(m_cfNamedMu);
            m_lastRouteOk     = ok;
            m_lastRouteResult = ok
                ? "DNS route applied — " + h + " now points at this tunnel."
                : (err.empty() ? "DNS route failed (unknown error)" : err);
            m_routeInFlight.store(false);
        }).detach();
    }
    ImGui::EndDisabled();
    if (currentTunnelId.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("(select a tunnel first)");
    }

    {
        std::lock_guard<std::mutex> lock(m_cfNamedMu);
        if (!m_lastRouteResult.empty())
        {
            ImVec4 col = m_lastRouteOk
                ? ImVec4(0.40f, 0.80f, 0.40f, 1.0f)
                : ImVec4(1.0f, 0.45f, 0.40f, 1.0f);
            ImGui::Spacing();
            ImGui::TextColored(col, "%s", m_lastRouteResult.c_str());
        }
    }

    ImGui::PopID();
}
