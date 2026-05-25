#include "UIConfigurationAudio.h"
#include "../utils/TranslationManager.h"
#include "UIManager.h"
#include "UISectionHeader.h"
#include "../audio/IAudioCapture.h"
#ifdef __linux__
#include "../audio/AudioCapturePulse.h"
#endif
#ifdef __APPLE__
#include "../audio/AudioCaptureCoreAudio.h"
#endif
#include <imgui.h>
#include <algorithm>

UIConfigurationAudio::UIConfigurationAudio(UIManager *uiManager)
    : m_uiManager(uiManager)
{
    if (uiManager)
    {
        m_audioCapture = uiManager->getAudioCapture();
    }
}

UIConfigurationAudio::~UIConfigurationAudio()
{
}

void UIConfigurationAudio::render()
{
    if (!m_visible || !m_uiManager) return;

    ImGui::SetNextWindowSize(ImVec2(520, 380), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(T("audio.title").c_str(), &m_visible))
    {
        ImGui::End();
        return;
    }

    m_audioCapture = m_uiManager->getAudioCapture();

    if (!m_audioCapture)
    {
        ImGui::TextWrapped("%s", T("audio.unavailable").c_str());
        ImGui::End();
        return;
    }

    renderInputSourceSelection();
#ifdef __APPLE__
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    renderAVFoundationAudioDeviceSelection();
#endif

    ImGui::End();
}

void UIConfigurationAudio::refreshInputSources()
{
    m_inputSourceNames.clear();
    m_inputSourceIds.clear();
    m_selectedInputSourceIndex = -1;

    if (!m_audioCapture)
    {
        return;
    }

#ifdef __linux__
    AudioCapturePulse *pulseCapture = dynamic_cast<AudioCapturePulse *>(m_audioCapture);
    if (pulseCapture)
    {
        // Get list of available input sources
        std::vector<AudioDeviceInfo> sources = pulseCapture->listInputSources();

        for (const auto &source : sources)
        {
            m_inputSourceNames.push_back(source.name);
            m_inputSourceIds.push_back(source.id);
        }

        // Try to find current input source
        std::string currentSource = pulseCapture->getCurrentInputSource();
        if (!currentSource.empty())
        {
            for (size_t i = 0; i < m_inputSourceIds.size(); ++i)
            {
                if (m_inputSourceIds[i] == currentSource)
                {
                    m_selectedInputSourceIndex = static_cast<int>(i);
                    break;
                }
            }
        }

        // If no active connection found, check saved configuration from UIManager
        if (m_selectedInputSourceIndex == -1 && m_uiManager)
        {
            std::string savedInputSourceId = m_uiManager->getAudioInputSourceId();
            if (!savedInputSourceId.empty())
            {
                for (size_t i = 0; i < m_inputSourceIds.size(); ++i)
                {
                    if (m_inputSourceIds[i] == savedInputSourceId)
                    {
                        m_selectedInputSourceIndex = static_cast<int>(i);
                        break;
                    }
                }
            }
        }
    }
#elif defined(__APPLE__)
    // Core Audio devices via IAudioCapture::listDevices — same listing
    // used elsewhere. On macOS the saved id comes from
    // `getAudioInputSourceId()`, same field reused.
    const auto devices = m_audioCapture->listDevices();
    for (const auto &d : devices)
    {
        m_inputSourceNames.push_back(d.name);
        m_inputSourceIds.push_back(d.id);
    }

    const std::string saved = m_uiManager
        ? m_uiManager->getAudioInputSourceId()
        : std::string();
    if (!saved.empty())
    {
        for (size_t i = 0; i < m_inputSourceIds.size(); ++i)
        {
            if (m_inputSourceIds[i] == saved)
            {
                m_selectedInputSourceIndex = static_cast<int>(i);
                break;
            }
        }
    }
#endif

    m_inputSourcesListNeedsRefresh = false;
}


void UIConfigurationAudio::renderInputSourceSelection()
{
    ui_section_header("Audio input",
                      "Pick the capture-card audio device. RetroCapture "
                      "records directly from it and republishes the "
                      "stream as the 'RetroCapture' source so other apps "
                      "(DAWs, monitors) can pick it up.");

    if (!m_audioCapture)
    {
        ImGui::TextWrapped("Audio capture not available.");
        return;
    }

    // Refresh button
    if (ImGui::Button("Refresh Input Sources"))
    {
        m_inputSourcesListNeedsRefresh = true;
    }

    ImGui::SameLine();
    if (m_inputSourcesListNeedsRefresh)
    {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Refreshing...");
    }

    ImGui::Spacing();

    // Refresh sources if needed
    if (m_inputSourcesListNeedsRefresh)
    {
        refreshInputSources();
    }

#ifdef __linux__
    AudioCapturePulse *pulseCapture = dynamic_cast<AudioCapturePulse *>(m_audioCapture);
    if (!pulseCapture)
    {
        ImGui::TextWrapped("Audio source selection is only available on Linux with PulseAudio.");
        return;
    }

    if (m_inputSourceNames.empty())
    {
        ImGui::TextWrapped("No audio input sources found. Make sure PulseAudio is running and audio devices are available.");
        return;
    }

    // Create C-style array for ImGui Combo
    std::vector<const char *> sourceNamesCStr;
    for (const auto &name : m_inputSourceNames)
    {
        sourceNamesCStr.push_back(name.c_str());
    }

    // Device dropdown drives the input directly — no more loopback hop
    // through a virtual sink.
    int prevSelectedIndex = m_selectedInputSourceIndex;
    if (ImGui::Combo("Input device", &m_selectedInputSourceIndex, sourceNamesCStr.data(), static_cast<int>(sourceNamesCStr.size())))
    {
        if (m_selectedInputSourceIndex >= 0 && m_selectedInputSourceIndex < static_cast<int>(m_inputSourceIds.size()))
        {
            std::string selectedSourceId = m_inputSourceIds[m_selectedInputSourceIndex];

            if (pulseCapture->connectInputSource(selectedSourceId))
            {
                if (m_uiManager)
                {
                    m_uiManager->setAudioInputSourceId(selectedSourceId);
                    m_uiManager->saveConfig();
                }
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Failed to open input device");
                m_selectedInputSourceIndex = prevSelectedIndex;
            }
        }
    }

    ImGui::Spacing();

    std::string currentSource = pulseCapture->getCurrentInputSource();
    if (!currentSource.empty())
    {
        ImGui::Text("Capturing from: %s", currentSource.c_str());
        ImGui::Text("Format: %u Hz, %u channel%s",
                    pulseCapture->getSampleRate(),
                    pulseCapture->getChannels(),
                    pulseCapture->getChannels() == 1 ? "" : "s");
        ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f),
                           "Published as 'RetroCapture' source");

        ImGui::Spacing();
        if (ImGui::Button("Resync monitor"))
        {
            pulseCapture->resyncMonitor();
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop capture"))
        {
            pulseCapture->disconnectInputSource();
            m_selectedInputSourceIndex = -1;
            if (m_uiManager)
            {
                m_uiManager->setAudioInputSourceId("");
                m_uiManager->saveConfig();
            }
        }
    }
    else
    {
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.0f, 1.0f), "No input device selected");
        ImGui::TextWrapped("Pick a device above to start capturing. "
                           "RetroCapture will record from it and publish "
                           "the audio as the 'RetroCapture' source.");
    }
#elif defined(__APPLE__)
    if (m_inputSourceNames.empty())
    {
        ImGui::TextWrapped("No Core Audio input devices found.");
        return;
    }

    std::vector<const char *> namesCStr;
    namesCStr.reserve(m_inputSourceNames.size());
    for (const auto &n : m_inputSourceNames) namesCStr.push_back(n.c_str());

    const int prev = m_selectedInputSourceIndex;
    int        idx = (m_selectedInputSourceIndex >= 0)
                     ? m_selectedInputSourceIndex : 0;
    if (ImGui::Combo("Input device", &idx,
                     namesCStr.data(), static_cast<int>(namesCStr.size())))
    {
        if (idx >= 0 && idx < static_cast<int>(m_inputSourceIds.size()))
        {
            const std::string pickedId = m_inputSourceIds[idx];
            m_audioCapture->close();
            if (m_audioCapture->open(pickedId))
            {
                m_audioCapture->startCapture();
                m_selectedInputSourceIndex = idx;
                if (m_uiManager)
                {
                    m_uiManager->setAudioInputSourceId(pickedId);
                    m_uiManager->saveConfig();
                }
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                                   "Failed to open Core Audio device");
                m_selectedInputSourceIndex = prev;
            }
        }
    }

    ImGui::Spacing();
    if (m_audioCapture->isOpen())
    {
        ImGui::Text("Format: %u Hz, %u channel%s",
                    m_audioCapture->getSampleRate(),
                    m_audioCapture->getChannels(),
                    m_audioCapture->getChannels() == 1 ? "" : "s");

        if (ImGui::Button("Resync monitor"))
        {
            if (auto *caCapture = dynamic_cast<AudioCaptureCoreAudio *>(m_audioCapture))
            {
                caCapture->resyncMonitor();
            }
        }
    }
#endif
}

#ifdef __APPLE__

void UIConfigurationAudio::renderAVFoundationAudioDeviceSelection()
{
    ui_section_header("AVFoundation audio source",
                      "Some capture devices carry audio alongside video "
                      "(UVC HDMI grabbers). Use this dropdown to pick "
                      "between the bundled stream and any other "
                      "AVFoundation audio source on the system.");

    if (!m_uiManager) return;

    // Listing the AVFoundation audio devices goes through the video
    // capture instance — AVFoundation owns the audio enumeration since
    // the audio is bundled with the video device descriptor.
    IVideoCapture *vc = m_uiManager->getCapture();
    if (!vc)
    {
        ImGui::TextWrapped("No AVFoundation capture active.");
        return;
    }

    if (m_avfAudioDevicesNeedRefresh)
    {
        m_avfAudioDevices = vc->listAudioDevices();
        m_avfAudioDevicesNeedRefresh = false;
    }

    if (m_avfAudioDevices.empty())
    {
        ImGui::TextWrapped("No AVFoundation audio devices found.");
        if (ImGui::Button("Refresh##avfAudioDevices"))
        {
            m_avfAudioDevicesNeedRefresh = true;
        }
        return;
    }

    const std::string current = vc->getCurrentAudioDevice();
    std::vector<const char *> names;
    int idx = -1;
    names.reserve(m_avfAudioDevices.size());
    for (size_t i = 0; i < m_avfAudioDevices.size(); ++i)
    {
        names.push_back(m_avfAudioDevices[i].name.c_str());
        if (m_avfAudioDevices[i].id == current) idx = static_cast<int>(i);
    }
    if (idx < 0) idx = 0;

    if (ImGui::Combo("##avfAudioDevices", &idx,
                     names.data(), static_cast<int>(names.size())))
    {
        const auto &picked = m_avfAudioDevices[idx];
        if (vc->setAudioDevice(picked.id))
        {
            m_uiManager->setAVFoundationAudioDeviceId(picked.id);
            m_uiManager->saveConfig();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh##avfAudioDevices"))
    {
        m_avfAudioDevicesNeedRefresh = true;
    }
}

#endif // __APPLE__

