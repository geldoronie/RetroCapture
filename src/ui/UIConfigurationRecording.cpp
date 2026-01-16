#include "UIConfigurationRecording.h"
#include "UIManager.h"
#include <imgui.h>
#include <algorithm>
#include <iomanip>
#include <sstream>

UIConfigurationRecording::UIConfigurationRecording(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UIConfigurationRecording::~UIConfigurationRecording()
{
}

void UIConfigurationRecording::render()
{
    if (!m_uiManager)
    {
        return;
    }

    renderRecordingStatus();
    ImGui::Separator();
    renderBasicSettings();
    ImGui::Separator();
    renderCodecSettings();
    ImGui::Separator();
    renderBitrateSettings();
    ImGui::Separator();
    renderContainerSettings();
    ImGui::Separator();
    renderOutputSettings();
    ImGui::Separator();
    renderStartStopButton();
}

void UIConfigurationRecording::renderRecordingStatus()
{
    ImGui::Text("Video Recording");
    ImGui::Separator();

    // Status
    bool active = m_uiManager->getRecordingActive();
    ImGui::Text("Status: %s", active ? "Recording" : "Stopped");
    if (active)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "●");
    }
    else
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "●");
    }

    if (active)
    {
        // Duration
        uint64_t durationUs = m_uiManager->getRecordingDurationUs();
        uint64_t seconds = durationUs / 1000000;
        uint64_t minutes = seconds / 60;
        seconds %= 60;
        uint64_t hours = minutes / 60;
        minutes %= 60;
        
        std::stringstream ss;
        ss << std::setfill('0') << std::setw(2) << hours << ":"
           << std::setw(2) << minutes << ":"
           << std::setw(2) << seconds;
        ImGui::Text("Duration: %s", ss.str().c_str());

        // File size
        uint64_t fileSize = m_uiManager->getRecordingFileSize();
        double sizeMB = static_cast<double>(fileSize) / (1024.0 * 1024.0);
        ImGui::Text("File Size: %.2f MB", sizeMB);

        // Filename
        std::string filename = m_uiManager->getRecordingFilename();
        if (!filename.empty())
        {
            ImGui::Text("File: %s", filename.c_str());
        }
    }
}

void UIConfigurationRecording::renderBasicSettings()
{
    ImGui::Text("Basic Settings");
    ImGui::Separator();

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
}

void UIConfigurationRecording::renderCodecSettings()
{
    ImGui::Text("Codecs");
    ImGui::Separator();

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

    // Include audio checkbox
    bool includeAudio = m_uiManager->getRecordingIncludeAudio();
    if (ImGui::Checkbox("Include Audio", &includeAudio))
    {
        m_uiManager->triggerRecordingIncludeAudioChange(includeAudio);
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
    ImGui::Text("Bitrate Settings");
    ImGui::Separator();

    // Video bitrate (in Mbps)
    uint32_t bitrate = m_uiManager->getRecordingBitrate();
    float bitrateMbps = static_cast<float>(bitrate) / 1000000.0f;
    if (ImGui::SliderFloat("Video Bitrate (Mbps)", &bitrateMbps, 1.0f, 50.0f, "%.1f"))
    {
        m_uiManager->triggerRecordingBitrateChange(static_cast<uint32_t>(bitrateMbps * 1000000));
    }

    // Audio bitrate (in kbps)
    uint32_t audioBitrate = m_uiManager->getRecordingAudioBitrate();
    float audioBitrateKbps = static_cast<float>(audioBitrate) / 1000.0f;
    if (ImGui::SliderFloat("Audio Bitrate (kbps)", &audioBitrateKbps, 64.0f, 320.0f, "%.0f"))
    {
        m_uiManager->triggerRecordingAudioBitrateChange(static_cast<uint32_t>(audioBitrateKbps * 1000));
    }
}

void UIConfigurationRecording::renderContainerSettings()
{
    ImGui::Text("Container Format");
    ImGui::Separator();

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
    ImGui::Text("Output Settings");
    ImGui::Separator();

    // Output path
    std::string outputPath = m_uiManager->getRecordingOutputPath();
    char pathBuffer[512];
    strncpy(pathBuffer, outputPath.c_str(), sizeof(pathBuffer) - 1);
    pathBuffer[sizeof(pathBuffer) - 1] = '\0';
    
    if (ImGui::InputText("Output Directory", pathBuffer, sizeof(pathBuffer)))
    {
        m_uiManager->triggerRecordingOutputPathChange(std::string(pathBuffer));
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
    ImGui::Separator();
    ImGui::Spacing();

    bool isRecording = m_uiManager->getRecordingActive();
    
    if (isRecording)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.8f, 0.0f, 0.0f, 1.0f));
        
        if (ImGui::Button("Stop Recording", ImVec2(-1, 40)))
        {
            m_uiManager->triggerRecordingStartStop(false);
        }
        
        ImGui::PopStyleColor(3);
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.7f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.9f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.0f, 0.5f, 0.0f, 1.0f));
        
        if (ImGui::Button("Start Recording", ImVec2(-1, 40)))
        {
            m_uiManager->triggerRecordingStartStop(true);
        }
        
        ImGui::PopStyleColor(3);
    }
}
