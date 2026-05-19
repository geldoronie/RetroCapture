#include "UIConfigurationRecording.h"
#include "../utils/TranslationManager.h"
#include "UIManager.h"
#include "UISectionHeader.h"
#include "../encoding/MediaEncoder.h"
#include "../utils/Logger.h"
#include <imgui.h>
#include <algorithm>
#include <functional>
#include <iomanip>
#include <sstream>
#include <vector>

UIConfigurationRecording::UIConfigurationRecording(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UIConfigurationRecording::~UIConfigurationRecording()
{
}

void UIConfigurationRecording::render()
{
    if (!m_visible || !m_uiManager) return;

    // Same window dims as Configuration → Streaming so the two feel
    // like the same family of dialog.
    ImGui::SetNextWindowSize(ImVec2(680, 720), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("recording.title").c_str(), &m_visible))
    {
        ImGui::End();
        return;
    }

    renderRecordingStatus();
    renderProfiles();
    {
        ui_section_header("Pipeline",
                          "Choose which feed lands on disk: the raw "
                          "source, or the post-shader output.");
        bool apply = m_uiManager->getRecordingApplyShader();
        if (ImGui::Checkbox("Apply shader to recording", &apply))
        {
            m_uiManager->setRecordingApplyShader(apply);
            m_uiManager->saveConfig();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("When off, the recording captures the raw pre-shader source\n"
                              "even though the live preview shows the shader applied.\n"
                              "Useful to archive a clean master while keeping the CRT\n"
                              "look on screen / on stream.");
        }
    }
    renderBasicSettings();
    renderCodecSettings();
    renderBitrateSettings();
    renderContainerSettings();
    renderOutputSettings();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    renderStartStopButton();

    ImGui::End();
}

void UIConfigurationRecording::renderRecordingStatus()
{
    ui_section_header("Video Recording");
    const bool active = m_uiManager->getRecordingActive();
    ui_status_indicator(active, "Recording", "Stopped");

    if (active)
    {
        const uint64_t durationUs = m_uiManager->getRecordingDurationUs();
        uint64_t seconds = durationUs / 1000000;
        uint64_t minutes = seconds / 60;
        seconds %= 60;
        const uint64_t hours = minutes / 60;
        minutes %= 60;

        std::stringstream ss;
        ss << std::setfill('0') << std::setw(2) << hours << ":"
           << std::setw(2) << minutes << ":"
           << std::setw(2) << seconds;
        ImGui::Text("Duration: %s", ss.str().c_str());

        const uint64_t fileSize = m_uiManager->getRecordingFileSize();
        const double   sizeMB   = static_cast<double>(fileSize) / (1024.0 * 1024.0);
        ImGui::Text("File Size: %.2f MB", sizeMB);

        const std::string filename = m_uiManager->getRecordingFilename();
        if (!filename.empty())
        {
            ImGui::Text("File: %s", filename.c_str());
        }
    }
}

void UIConfigurationRecording::refreshProfiles()
{
    m_profileNames = m_uiManager->listRecordingProfiles();
    m_profilesDirty = false;
    if (m_selectedProfileIndex >= static_cast<int>(m_profileNames.size()))
    {
        m_selectedProfileIndex = m_profileNames.empty() ? -1 : 0;
    }
}

void UIConfigurationRecording::renderProfiles()
{
    if (m_profilesDirty) refreshProfiles();

    ui_section_header("Profiles",
                      "Saved encoder + container configurations you can "
                      "swap between.");

    const char *currentLabel = (m_selectedProfileIndex >= 0 &&
                                m_selectedProfileIndex < static_cast<int>(m_profileNames.size()))
                                   ? m_profileNames[m_selectedProfileIndex].c_str()
                                   : "(no profile selected)";

    if (ImGui::BeginCombo("##recording_profile", currentLabel))
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
        if (!m_uiManager->loadRecordingProfile(name))
        {
            LOG_ERROR("Failed to load recording profile: " + name);
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

    // Save dialog
    if (m_showSaveDialog) ImGui::OpenPopup("Save Recording Profile");
    if (ImGui::BeginPopupModal("Save Recording Profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Profile name:");
        ImGui::InputText("##profile_name", m_newProfileName, sizeof(m_newProfileName));

        std::string nameStr(m_newProfileName);
        bool exists = !nameStr.empty() && m_uiManager->recordingProfileExists(nameStr);
        if (exists)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "A profile with this name already exists.");
        }

        if (ImGui::Button("Save", ImVec2(120, 0)))
        {
            if (!nameStr.empty())
            {
                if (m_uiManager->saveRecordingProfile(nameStr))
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

    // Delete confirm
    if (m_showDeleteConfirm) ImGui::OpenPopup("Delete Recording Profile");
    if (ImGui::BeginPopupModal("Delete Recording Profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (m_selectedProfileIndex >= 0 && m_selectedProfileIndex < static_cast<int>(m_profileNames.size()))
        {
            ImGui::Text("Delete profile \"%s\"?", m_profileNames[m_selectedProfileIndex].c_str());
        }
        if (ImGui::Button("Delete", ImVec2(120, 0)))
        {
            if (m_selectedProfileIndex >= 0 && m_selectedProfileIndex < static_cast<int>(m_profileNames.size()))
            {
                m_uiManager->deleteRecordingProfile(m_profileNames[m_selectedProfileIndex]);
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

void UIConfigurationRecording::renderBasicSettings()
{
    ui_section_header("Video",
                      "Output resolution and frame rate. \"Capture\" "
                      "keeps whatever the source delivers.");

    // Resolution - Dropdown
    const char *resolutions[] = {
        "Capture (0x0)",
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

    uint32_t currentWidth = m_uiManager->getRecordingWidth();
    uint32_t currentHeight = m_uiManager->getRecordingHeight();
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
        m_uiManager->triggerRecordingWidthChange(resolutionWidths[currentResIndex]);
        m_uiManager->triggerRecordingHeightChange(resolutionHeights[currentResIndex]);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Output resolution written to the file.\n"
                          "\"Capture\" leaves it at whatever the source\n"
                          "produces (recommended for retro consoles to\n"
                          "preserve native resolution).");
    }

    // FPS - Dropdown
    const char *fpsOptions[] = {"Capture (0)", "15", "24", "30", "60", "120"};
    const uint32_t fpsValues[] = {0, 15, 24, 30, 60, 120};

    uint32_t currentFps = m_uiManager->getRecordingFps();
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
        m_uiManager->triggerRecordingFpsChange(fpsValues[currentFpsIndex]);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Output frame rate. \"Capture\" matches the source.\n"
                          "Higher than source upscales temporally and wastes bits;\n"
                          "lower than source drops frames.");
    }
}

void UIConfigurationRecording::renderCodecSettings()
{
    ui_section_header("Codecs",
                      "What encodes the video and audio. Codec-specific "
                      "tuning appears below the dropdown.");

    // Video codec selection
    const char *videoCodecs[] = {"h264", "h265", "vp8", "vp9"};
    std::string currentVideoCodec = m_uiManager->getRecordingVideoCodec();
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
        m_uiManager->triggerRecordingVideoCodecChange(videoCodecs[currentVideoCodecIndex]);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("h264: universal compatibility, default.\n"
                          "h265: ~30%% smaller files at the same quality,\n"
                          "      but slower to encode and not all players\n"
                          "      handle it (DaVinci/older Premiere can).\n"
                          "vp8/vp9: WebM-friendly, mkv container only.");
    }

    // Hardware encoder selector + backend-specific preset (#59).
    // Mirror of UIConfigurationStreaming — same options, same Auto
    // fallback, defaults are tuned for files (NVENC p4, VAAPI VBR,
    // AMF quality) instead of streaming. Only shown for H.264/HEVC;
    // VP8/VP9 have no hardware backends we support.
    if (currentVideoCodec == "h264" || currentVideoCodec == "h265" || currentVideoCodec == "hevc")
    {
        std::vector<MediaEncoder::HardwareEncoder> available = MediaEncoder::detectAvailableEncoders();
        std::vector<MediaEncoder::HardwareEncoder> options;
        options.push_back(MediaEncoder::HardwareEncoder::Auto);
        for (auto h : available) options.push_back(h);

        std::vector<const char *> labels;
        labels.reserve(options.size());
        for (auto h : options) labels.push_back(MediaEncoder::hardwareEncoderName(h));

        const int current = m_uiManager->getRecordingHardwareEncoder();
        int currentIndex = 0;
        for (size_t i = 0; i < options.size(); ++i)
        {
            if (static_cast<int>(options[i]) == current) { currentIndex = static_cast<int>(i); break; }
        }
        if (ImGui::Combo("Encoder", &currentIndex, labels.data(), static_cast<int>(labels.size())))
        {
            m_uiManager->triggerRecordingHardwareEncoderChange(static_cast<int>(options[currentIndex]));
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Auto = try hardware (NVENC/VAAPI/QSV/AMF), fall back to software on failure.\n"
                              "Software = libx264 guaranteed on any machine.\n"
                              "Hardware backends only show up when ffmpeg was built with support and\n"
                              "may fail at runtime if the driver/permission is missing — in that\n"
                              "case the recording falls back to libx264 automatically.");
        }

        const MediaEncoder::HardwareEncoder activeHw = options[currentIndex];
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
            renderEnumCombo("NVENC Preset", items, 7, m_uiManager->getRecordingNvencPreset(),
                            [this](const std::string &v) { m_uiManager->triggerRecordingNvencPresetChange(v); },
                            "p1 = fastest (lowest quality) ... p7 = slowest (highest quality).\n"
                            "p4 is the recommended balance; p5–p6 are fine for files where\n"
                            "you can afford extra encoder latency.");
        }
        else if (activeHw == MediaEncoder::HardwareEncoder::VAAPI)
        {
            static const char *items[] = {"CBR", "VBR", "CQP"};
            renderEnumCombo("VAAPI Rate Control", items, 3, m_uiManager->getRecordingVaapiRcMode(),
                            [this](const std::string &v) { m_uiManager->triggerRecordingVaapiRcModeChange(v); },
                            "CBR = constant bitrate.\n"
                            "VBR = variable bitrate (recommended for files — sharper highlights).\n"
                            "CQP = constant quality (ignores bitrate slider).");
        }
        else if (activeHw == MediaEncoder::HardwareEncoder::QSV)
        {
            static const char *items[] = {"veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"};
            renderEnumCombo("QSV Preset", items, 7, m_uiManager->getRecordingQsvPreset(),
                            [this](const std::string &v) { m_uiManager->triggerRecordingQsvPresetChange(v); },
                            "Intel Quick Sync presets. Faster = lower quality.\n"
                            "For files, medium / slow is a reasonable balance.");
        }
        else if (activeHw == MediaEncoder::HardwareEncoder::AMF)
        {
            static const char *items[] = {"speed", "balanced", "quality"};
            renderEnumCombo("AMF Quality", items, 3, m_uiManager->getRecordingAmfQuality(),
                            [this](const std::string &v) { m_uiManager->triggerRecordingAmfQualityChange(v); },
                            "speed = minimum latency, basic quality.\n"
                            "balanced = middle ground.\n"
                            "quality = best visual, higher latency (recommended for files).");
        }
    }

    // Audio codec selection
    const char *audioCodecs[] = {"aac", "mp3", "opus"};
    std::string currentAudioCodec = m_uiManager->getRecordingAudioCodec();
    int currentAudioCodecIndex = 0;
    for (int i = 0; i < 3; i++)
    {
        if (currentAudioCodec == audioCodecs[i])
        {
            currentAudioCodecIndex = i;
            break;
        }
    }

    if (ImGui::Combo("Audio Codec", &currentAudioCodecIndex, audioCodecs, 3))
    {
        m_uiManager->triggerRecordingAudioCodecChange(audioCodecs[currentAudioCodecIndex]);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("aac: best compatibility, default for mp4.\n"
                          "mp3: legacy, lossy at very low bitrates.\n"
                          "opus: highest quality per bit, mkv/webm only.");
    }

    // Include audio checkbox
    bool includeAudio = m_uiManager->getRecordingIncludeAudio();
    if (ImGui::Checkbox("Include Audio", &includeAudio))
    {
        m_uiManager->triggerRecordingIncludeAudioChange(includeAudio);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Off = silent recording. The capture pipeline still\n"
                          "samples audio for the stream, but the muxer skips\n"
                          "the audio track entirely on disk.");
    }

    // Codec-specific settings
    if (currentVideoCodec == "h264")
    {
        renderH264Settings();
    }

    if (currentVideoCodec == "h265" || currentVideoCodec == "hevc")
    {
        renderH265Settings();
    }

    if (currentVideoCodec == "vp8")
    {
        renderVP8Settings();
    }

    if (currentVideoCodec == "vp9")
    {
        renderVP9Settings();
    }
}

void UIConfigurationRecording::renderH264Settings()
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
    std::string currentPreset = m_uiManager->getRecordingH264Preset();
    int currentPresetIndex = 2; // Default: veryfast
    for (int i = 0; i < 9; i++)
    {
        if (currentPreset == h264Presets[i])
        {
            currentPresetIndex = i;
            break;
        }
    }

    if (ImGui::Combo("H.264 Preset", &currentPresetIndex, h264Presets, 9))
    {
        m_uiManager->triggerRecordingH264PresetChange(h264Presets[currentPresetIndex]);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("H.264 encoder preset:\n"
                          "ultrafast/superfast/veryfast: Maximum speed, lower quality\n"
                          "fast/medium: Balance between speed and quality\n"
                          "slow/slower/veryslow: Maximum quality, lower speed");
    }
}

void UIConfigurationRecording::renderH265Settings()
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
    std::string currentPreset = m_uiManager->getRecordingH265Preset();
    int currentPresetIndex = 2; // Default: veryfast
    for (int i = 0; i < 9; i++)
    {
        if (currentPreset == h265Presets[i])
        {
            currentPresetIndex = i;
            break;
        }
    }

    if (ImGui::Combo("H.265 Preset", &currentPresetIndex, h265Presets, 9))
    {
        m_uiManager->triggerRecordingH265PresetChange(h265Presets[currentPresetIndex]);
    }

    // H.265 Profile
    const char *h265Profiles[] = {"main", "main10"};
    std::string currentProfile = m_uiManager->getRecordingH265Profile();
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
        m_uiManager->triggerRecordingH265ProfileChange(h265Profiles[currentProfileIndex]);
    }

    // H.265 Level
    const char *h265Levels[] = {
        "auto", "1", "2", "2.1", "3", "3.1",
        "4", "4.1", "5", "5.1", "5.2", "6", "6.1", "6.2"};
    std::string currentLevel = m_uiManager->getRecordingH265Level();
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
        m_uiManager->triggerRecordingH265LevelChange(h265Levels[currentLevelIndex]);
    }
}

void UIConfigurationRecording::renderVP8Settings()
{
    int speed = m_uiManager->getRecordingVP8Speed();
    if (ImGui::SliderInt("VP8 Speed", &speed, 0, 16))
    {
        m_uiManager->triggerRecordingVP8SpeedChange(speed);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("VP8 encoding speed (0-16):\n"
                          "Higher values = faster encoding, lower quality");
    }
}

void UIConfigurationRecording::renderVP9Settings()
{
    int speed = m_uiManager->getRecordingVP9Speed();
    if (ImGui::SliderInt("VP9 Speed", &speed, 0, 9))
    {
        m_uiManager->triggerRecordingVP9SpeedChange(speed);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("VP9 encoding speed (0-9):\n"
                          "Higher values = faster encoding, lower quality");
    }
}

void UIConfigurationRecording::renderBitrateSettings()
{
    ui_section_header("Bitrate",
                      "How aggressively the encoder spends bits. Higher "
                      "= better quality, larger files; lower = smaller "
                      "files, visible compression artefacts.");

    // Video bitrate (in Mbps)
    uint32_t bitrate = m_uiManager->getRecordingBitrate();
    float bitrateMbps = static_cast<float>(bitrate) / 1000000.0f;
    if (ImGui::SliderFloat("Video Bitrate (Mbps)", &bitrateMbps, 1.0f, 50.0f, "%.1f"))
    {
        m_uiManager->triggerRecordingBitrateChange(static_cast<uint32_t>(bitrateMbps * 1000000));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Recommended starting points:\n"
                          " ~3 Mbps  — 720p archival\n"
                          " ~6 Mbps  — 1080p archival\n"
                          " ~12 Mbps — 1080p HEVC at zero-compromise\n"
                          " ~25 Mbps — 4K archival");
    }

    // Audio bitrate (in kbps)
    uint32_t audioBitrate = m_uiManager->getRecordingAudioBitrate();
    float audioBitrateKbps = static_cast<float>(audioBitrate) / 1000.0f;
    if (ImGui::SliderFloat("Audio Bitrate (kbps)", &audioBitrateKbps, 64.0f, 320.0f, "%.0f"))
    {
        m_uiManager->triggerRecordingAudioBitrateChange(static_cast<uint32_t>(audioBitrateKbps * 1000));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("128 kbps is transparent for AAC; 256 kbps is\n"
                          "the safe default. Opus reaches the same\n"
                          "quality around 96 kbps.");
    }
}

void UIConfigurationRecording::renderContainerSettings()
{
    ui_section_header("Container",
                      "The file format that wraps the encoded streams.");

    const char *containers[] = {"mp4", "mkv", "avi"};
    std::string currentContainer = m_uiManager->getRecordingContainer();
    int currentContainerIndex = 0;
    for (int i = 0; i < 3; i++)
    {
        if (currentContainer == containers[i])
        {
            currentContainerIndex = i;
            break;
        }
    }

    if (ImGui::Combo("Container", &currentContainerIndex, containers, 3))
    {
        m_uiManager->triggerRecordingContainerChange(containers[currentContainerIndex]);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Container format:\n"
                          "mp4: Best compatibility\n"
                          "mkv: Supports more codecs\n"
                          "avi: Legacy format");
    }
}

void UIConfigurationRecording::renderOutputSettings()
{
    ui_section_header("Output",
                      "Where files land and how they're named. Template "
                      "uses strftime tokens — see the field tooltip.");

    // Output path
    std::string outputPath = m_uiManager->getRecordingOutputPath();
    char pathBuffer[512];
    strncpy(pathBuffer, outputPath.c_str(), sizeof(pathBuffer) - 1);
    pathBuffer[sizeof(pathBuffer) - 1] = '\0';
    
    if (ImGui::InputText("Output Directory", pathBuffer, sizeof(pathBuffer)))
    {
        m_uiManager->triggerRecordingOutputPathChange(std::string(pathBuffer));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Created automatically on first record. Relative\n"
                          "paths resolve against the application working\n"
                          "directory; absolute paths are honoured as-is.");
    }

    // Filename template
    std::string template_ = m_uiManager->getRecordingFilenameTemplate();
    char templateBuffer[256];
    strncpy(templateBuffer, template_.c_str(), sizeof(templateBuffer) - 1);
    templateBuffer[sizeof(templateBuffer) - 1] = '\0';
    
    if (ImGui::InputText("Filename Template", templateBuffer, sizeof(templateBuffer)))
    {
        m_uiManager->triggerRecordingFilenameTemplateChange(std::string(templateBuffer));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Filename template (strftime format):\n"
                          "Example: recording_%%Y%%m%%d_%%H%%M%%S\n"
                          "Will generate: recording_20241215_143022");
    }
}

void UIConfigurationRecording::renderStartStopButton()
{
    // Same `ImVec2(-1, 0)` full-width default-height button used by
    // UIConfigurationStreaming::renderStartStopButton — no colour
    // push, no oversized height. The two windows look like the same
    // dialog from across the room.
    const bool isRecording = m_uiManager->getRecordingActive();
    if (isRecording)
    {
        if (ImGui::Button("Stop Recording", ImVec2(-1, 0)))
        {
            m_uiManager->triggerRecordingStartStop(false);
        }
    }
    else
    {
        if (ImGui::Button("Start Recording", ImVec2(-1, 0)))
        {
            m_uiManager->triggerRecordingStartStop(true);
        }
    }
}
