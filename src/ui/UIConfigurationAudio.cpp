#include "UIConfigurationAudio.h"
#include "../utils/TranslationManager.h"
#include "UIManager.h"
#include "UISectionHeader.h"
#include "../audio/IAudioCapture.h"
#ifdef __linux__
#include "../audio/AudioCapturePulse.h"
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
#endif
}

