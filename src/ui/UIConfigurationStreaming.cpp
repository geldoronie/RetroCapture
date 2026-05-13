#include "UIConfigurationStreaming.h"
#include "UIManager.h"
#include "../utils/Logger.h"
#include "../encoding/MediaEncoder.h"
#include <imgui.h>
#include <algorithm>

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
    // Only relevant for H.264 — the other codecs in this project go
    // through their software encoders exclusively.
    if (currentVideoCodec == "h264")
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
