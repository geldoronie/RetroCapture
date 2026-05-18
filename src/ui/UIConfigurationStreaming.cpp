#include "UIConfigurationStreaming.h"
#include "UIManager.h"
#include "../utils/Logger.h"
#include "../encoding/MediaEncoder.h"
#include <imgui.h>
#include <algorithm>
#include <cstring>
#include <thread>

UIConfigurationStreaming::UIConfigurationStreaming(UIManager *uiManager)
    : m_uiManager(uiManager)
{
}

UIConfigurationStreaming::~UIConfigurationStreaming()
{
}

void UIConfigurationStreaming::render()
{
    if (!m_uiManager)
    {
        return;
    }

    renderStreamingStatus();
    ImGui::Separator();
    renderProfiles();
    ImGui::Separator();
    {
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
        ImGui::Separator();
    }
    renderBasicSettings();
    ImGui::Separator();
    renderCodecSettings();
    ImGui::Separator();
    renderBitrateSettings();
    ImGui::Separator();
    renderDirectoryPublish();      // #49 Phase 2
    ImGui::Separator();
    // Buffer tuning (max video/audio buffer, max buffer time, AVIO buffer)
    // is not surfaced in the UI anymore — defaults work for the vast
    // majority of cases. Power users can still override via config.json.
    renderStartStopButton();
}

void UIConfigurationStreaming::renderStreamingStatus()
{
    ImGui::Text("HTTP MPEG-TS Streaming (Áudio + Vídeo)");
    ImGui::Separator();

    // Status
    bool active = m_uiManager->getStreamingActive();
    ImGui::Text("Status: %s", active ? "Ativo" : "Inativo");
    if (active)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "●");
    }
    else
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "●");
    }

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

    ImGui::Text("Profiles");
    ImGui::Separator();

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
    ImGui::Text("Configurações Básicas");
    ImGui::Separator();

    // Porta
    int port = static_cast<int>(m_uiManager->getStreamingPort());
    if (ImGui::InputInt("Porta", &port, 1, 100))
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

    if (ImGui::Combo("Resolução", &currentResIndex, resolutions, 10))
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
    ImGui::Text("Codecs");
    ImGui::Separator();

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

    if (ImGui::Combo("Codec de Vídeo", &currentVideoCodecIndex, videoCodecs, 4))
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
            ImGui::SetTooltip("Auto = tenta hardware (NVENC/VAAPI/QSV/AMF) e cai pra software se falhar.\n"
                              "Software = libx264 garantido em qualquer máquina.\n"
                              "Backends de hardware só aparecem se o ffmpeg foi compilado com suporte e\n"
                              "podem falhar em runtime se driver/permissão estiverem ausentes — caso\n"
                              "isso aconteça, o stream volta a libx264 automaticamente.");
        }

        // Backend-specific quality / rate-control combo. libx264 keeps
        // its existing "Qualidade H.264" dropdown rendered below;
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
            renderEnumCombo("Preset NVENC", items, 7, m_uiManager->getStreamingNvencPreset(),
                            [this](const std::string &v) { m_uiManager->triggerStreamingNvencPresetChange(v); },
                            "p1 = fastest (qualidade mais baixa) ... p7 = slowest (melhor qualidade).\n"
                            "p4 é o equilíbrio recomendado para streaming em tempo real.");
        }
        else if (activeHw == MediaEncoder::HardwareEncoder::VAAPI)
        {
            static const char *items[] = {"CBR", "VBR", "CQP"};
            renderEnumCombo("Rate Control VAAPI", items, 3, m_uiManager->getStreamingVaapiRcMode(),
                            [this](const std::string &v) { m_uiManager->triggerStreamingVaapiRcModeChange(v); },
                            "CBR = bitrate constante (recomendado para streaming).\n"
                            "VBR = bitrate variável (melhor pra gravação).\n"
                            "CQP = qualidade fixa (ignora bitrate, qualidade constante).");
        }
        else if (activeHw == MediaEncoder::HardwareEncoder::QSV)
        {
            static const char *items[] = {"veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"};
            renderEnumCombo("Preset QSV", items, 7, m_uiManager->getStreamingQsvPreset(),
                            [this](const std::string &v) { m_uiManager->triggerStreamingQsvPresetChange(v); },
                            "Presets do Intel Quick Sync. Mais rápido = menos qualidade.\n"
                            "veryfast/faster são recomendados pra tempo real.");
        }
        else if (activeHw == MediaEncoder::HardwareEncoder::AMF)
        {
            static const char *items[] = {"speed", "balanced", "quality"};
            renderEnumCombo("Qualidade AMF", items, 3, m_uiManager->getStreamingAmfQuality(),
                            [this](const std::string &v) { m_uiManager->triggerStreamingAmfQualityChange(v); },
                            "speed = mínima latência, qualidade básica.\n"
                            "balanced = meio termo.\n"
                            "quality = melhor visual, latência maior.");
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

    if (ImGui::Combo("Qualidade H.264", &currentPresetIndex, h264Presets, 9))
    {
        m_uiManager->triggerStreamingH264PresetChange(h264Presets[currentPresetIndex]);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Preset do encoder H.264:\n"
                          "ultrafast/superfast/veryfast: Máxima velocidade, menor qualidade\n"
                          "fast/medium: Equilíbrio entre velocidade e qualidade\n"
                          "slow/slower/veryslow: Máxima qualidade, menor velocidade");
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

    if (ImGui::Combo("Qualidade H.265", &currentPresetIndex, h265Presets, 9))
    {
        m_uiManager->triggerStreamingH265PresetChange(h265Presets[currentPresetIndex]);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Preset do encoder H.265:\n"
                          "ultrafast/superfast/veryfast: Máxima velocidade, menor qualidade\n"
                          "fast/medium: Equilíbrio entre velocidade e qualidade\n"
                          "slow/slower/veryslow: Máxima qualidade, menor velocidade");
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

    if (ImGui::Combo("Profile H.265", &currentProfileIndex, h265Profiles, 2))
    {
        m_uiManager->triggerStreamingH265ProfileChange(h265Profiles[currentProfileIndex]);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Profile do encoder H.265:\n"
                          "main: 8-bit, máxima compatibilidade\n"
                          "main10: 10-bit, melhor qualidade, suporte HDR");
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

    if (ImGui::Combo("Level H.265", &currentLevelIndex, h265Levels, 14))
    {
        m_uiManager->triggerStreamingH265LevelChange(h265Levels[currentLevelIndex]);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Level do encoder H.265:\n"
                          "auto: Detecção automática (recomendado)\n"
                          "1-6.2: Níveis específicos para compatibilidade\n"
                          "Níveis mais altos suportam resoluções/bitrates maiores");
    }
}

void UIConfigurationStreaming::renderVP8Settings()
{
    int currentSpeed = m_uiManager->getStreamingVP8Speed();
    if (ImGui::SliderInt("Speed VP8 (0-16)", &currentSpeed, 0, 16))
    {
        m_uiManager->triggerStreamingVP8SpeedChange(currentSpeed);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Speed do encoder VP8:\n"
                          "0: Melhor qualidade, mais lento\n"
                          "16: Mais rápido, menor qualidade\n"
                          "12: Bom equilíbrio para streaming");
    }
}

void UIConfigurationStreaming::renderVP9Settings()
{
    int currentSpeed = m_uiManager->getStreamingVP9Speed();
    if (ImGui::SliderInt("Speed VP9 (0-9)", &currentSpeed, 0, 9))
    {
        m_uiManager->triggerStreamingVP9SpeedChange(currentSpeed);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Speed do encoder VP9:\n"
                          "0: Melhor qualidade, mais lento\n"
                          "9: Mais rápido, menor qualidade\n"
                          "6: Bom equilíbrio para streaming");
    }
}

void UIConfigurationStreaming::renderBitrateSettings()
{
    ImGui::Text("Bitrates");
    ImGui::Separator();

    // Bitrate de vídeo
    int bitrate = static_cast<int>(m_uiManager->getStreamingBitrate());
    if (ImGui::InputInt("Bitrate Vídeo (kbps, 0 = auto)", &bitrate, 100, 1000))
    {
        // Limites: 0 (auto) ou 100-100000 kbps
        if (bitrate == 0 || (bitrate >= 100 && bitrate <= 100000))
        {
            m_uiManager->triggerStreamingBitrateChange(static_cast<uint32_t>(bitrate));
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Bitrate de vídeo em kbps.\n"
                          "0 = automático (baseado na resolução/FPS)\n"
                          "100-100000 kbps: valores válidos\n"
                          "Recomendado: 2000-8000 kbps para streaming");
    }

    // Bitrate de áudio
    int audioBitrate = static_cast<int>(m_uiManager->getStreamingAudioBitrate());
    if (ImGui::InputInt("Bitrate Áudio (kbps)", &audioBitrate, 8, 32))
    {
        // Limites: 64-320 kbps (32 é muito baixo para qualidade aceitável)
        if (audioBitrate >= 64 && audioBitrate <= 320)
        {
            m_uiManager->triggerStreamingAudioBitrateChange(static_cast<uint32_t>(audioBitrate));
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Bitrate de áudio em kbps.\n"
                          "64-320 kbps: valores válidos\n"
                          "Recomendado: 128-256 kbps para boa qualidade");
    }
}

void UIConfigurationStreaming::renderAdvancedBufferSettings()
{
    ImGui::Text("Buffer (Avançado)");
    ImGui::Separator();

    // Max Video Buffer Size
    int maxVideoBuffer = static_cast<int>(m_uiManager->getStreamingMaxVideoBufferSize());
    if (ImGui::SliderInt("Max Frames no Buffer", &maxVideoBuffer, 1, 50))
    {
        m_uiManager->triggerStreamingMaxVideoBufferSizeChange(static_cast<size_t>(maxVideoBuffer));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Máximo de frames de vídeo no buffer.\n"
                          "1-50 frames: valores válidos\n"
                          "Padrão: 10 frames\n"
                          "Valores maiores = mais memória, menos risco de perda de frames");
    }

    // Max Audio Buffer Size
    int maxAudioBuffer = static_cast<int>(m_uiManager->getStreamingMaxAudioBufferSize());
    if (ImGui::SliderInt("Max Chunks no Buffer", &maxAudioBuffer, 5, 100))
    {
        m_uiManager->triggerStreamingMaxAudioBufferSizeChange(static_cast<size_t>(maxAudioBuffer));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Máximo de chunks de áudio no buffer.\n"
                          "5-100 chunks: valores válidos\n"
                          "Padrão: 20 chunks\n"
                          "Valores maiores = mais memória, melhor sincronização");
    }

    // Max Buffer Time
    int maxBufferTime = static_cast<int>(m_uiManager->getStreamingMaxBufferTimeSeconds());
    if (ImGui::SliderInt("Max Tempo de Buffer (segundos)", &maxBufferTime, 1, 30))
    {
        m_uiManager->triggerStreamingMaxBufferTimeSecondsChange(static_cast<int64_t>(maxBufferTime));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Tempo máximo de buffer em segundos.\n"
                          "1-30 segundos: valores válidos\n"
                          "Padrão: 5 segundos\n"
                          "Controla quanto tempo de vídeo/áudio pode ser armazenado antes de processar");
    }

    // AVIO Buffer Size
    int avioBuffer = static_cast<int>(m_uiManager->getStreamingAVIOBufferSize() / 1024); // Converter para KB
    if (ImGui::SliderInt("AVIO Buffer (KB)", &avioBuffer, 64, 1024))
    {
        m_uiManager->triggerStreamingAVIOBufferSizeChange(static_cast<size_t>(avioBuffer * 1024));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Tamanho do buffer AVIO do FFmpeg em KB.\n"
                          "64-1024 KB: valores válidos\n"
                          "Padrão: 256 KB\n"
                          "Buffer interno do FFmpeg para I/O de streaming");
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
        if (ImGui::Button("Parar Streaming", ImVec2(-1, 0)))
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
            if (ImGui::Button("Iniciar Streaming", ImVec2(-1, 0)))
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
    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "Public directory");
    ImGui::TextWrapped(
        "Optionally list this stream in a public directory so other "
        "RetroCapture clients can find it without you sharing the URL "
        "out of band.");
    ImGui::Spacing();

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
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", m_uiManager->getDirectoryHostNickname().c_str());
        if (ImGui::InputText("Nickname (optional)", buf, sizeof(buf)))
        {
            m_uiManager->setDirectoryHostNickname(buf);
            m_uiManager->saveConfig();
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

    // Advanced: directory URL override.
    if (ImGui::TreeNode("Advanced"))
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", m_uiManager->getDirectoryUrl().c_str());
        if (ImGui::InputText("Directory URL", buf, sizeof(buf)))
        {
            m_uiManager->setDirectoryUrl(buf);
            m_uiManager->saveConfig();
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
}
