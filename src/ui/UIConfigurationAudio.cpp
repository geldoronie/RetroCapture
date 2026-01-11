#include "UIConfigurationAudio.h"
#include "UIManager.h"
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
    if (!m_uiManager)
    {
        return;
    }

    // Update reference to audio capture if needed
    m_audioCapture = m_uiManager->getAudioCapture();

    if (!m_audioCapture)
    {
        ImGui::TextWrapped("Audio capture not available. Audio is required for streaming and recording.");
        return;
    }

    renderInputSourceSelection();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    renderOutputSinkSelection();
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
    }
#endif

    m_inputSourcesListNeedsRefresh = false;
}

void UIConfigurationAudio::refreshOutputSinks()
{
    m_outputSinkNames.clear();
    m_outputSinkIds.clear();
    m_selectedOutputSinkIndex = -1;

    if (!m_audioCapture)
    {
        return;
    }

#ifdef __linux__
    AudioCapturePulse *pulseCapture = dynamic_cast<AudioCapturePulse *>(m_audioCapture);
    if (pulseCapture)
    {
        // Get list of available output sinks
        std::vector<AudioDeviceInfo> sinks = pulseCapture->listOutputSinks();

        for (const auto &sink : sinks)
        {
            m_outputSinkNames.push_back(sink.name);
            m_outputSinkIds.push_back(sink.id);
        }

        // Try to find current monitoring output
        std::string currentOutput = pulseCapture->getCurrentMonitoringOutput();
        if (!currentOutput.empty())
        {
            for (size_t i = 0; i < m_outputSinkIds.size(); ++i)
            {
                if (m_outputSinkIds[i] == currentOutput)
                {
                    m_selectedOutputSinkIndex = static_cast<int>(i);
                    m_monitoringEnabled = true;
                    break;
                }
            }
        }
    }
#endif

    m_outputSinksListNeedsRefresh = false;
}

void UIConfigurationAudio::renderInputSourceSelection()
{
    ImGui::Text("Audio Input Source");
    ImGui::Separator();
    ImGui::TextWrapped("Select the audio source to capture. This audio will be recorded and streamed.");

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

    // Input source selection combo
    int prevSelectedIndex = m_selectedInputSourceIndex;
    if (ImGui::Combo("Input Source", &m_selectedInputSourceIndex, sourceNamesCStr.data(), static_cast<int>(sourceNamesCStr.size())))
    {
        // Source changed
        if (m_selectedInputSourceIndex >= 0 && m_selectedInputSourceIndex < static_cast<int>(m_inputSourceIds.size()))
        {
            std::string selectedSourceId = m_inputSourceIds[m_selectedInputSourceIndex];
            
            if (pulseCapture->connectInputSource(selectedSourceId))
            {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Input source connected successfully");
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Failed to connect input source");
                // Revert selection
                m_selectedInputSourceIndex = prevSelectedIndex;
            }
        }
    }

    ImGui::Spacing();

    // Current input source info
    std::string currentSource = pulseCapture->getCurrentInputSource();
    if (!currentSource.empty())
    {
        ImGui::Text("Current Input: %s", currentSource.c_str());
        
        ImGui::Spacing();
        if (ImGui::Button("Disconnect Input Source"))
        {
            pulseCapture->disconnectInputSource();
            m_selectedInputSourceIndex = -1;
        }
    }
    else
    {
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.0f, 1.0f), "No input source connected");
        ImGui::TextWrapped("Select an input source above to connect it to RetroCapture. "
                         "The selected source will be captured for streaming and recording.");
    }
#endif
}

void UIConfigurationAudio::renderOutputSinkSelection()
{
    ImGui::Text("Audio Output (Monitoring)");
    ImGui::Separator();
    ImGui::TextWrapped("Select an output sink to hear the captured audio in real-time.");

    if (!m_audioCapture)
    {
        ImGui::TextWrapped("Audio capture not available.");
        return;
    }

    // Refresh button
    if (ImGui::Button("Refresh Output Sinks"))
    {
        m_outputSinksListNeedsRefresh = true;
    }

    ImGui::SameLine();
    if (m_outputSinksListNeedsRefresh)
    {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Refreshing...");
    }

    ImGui::Spacing();

    // Refresh sinks if needed
    if (m_outputSinksListNeedsRefresh)
    {
        refreshOutputSinks();
    }

#ifdef __linux__
    AudioCapturePulse *pulseCapture = dynamic_cast<AudioCapturePulse *>(m_audioCapture);
    if (!pulseCapture)
    {
        ImGui::TextWrapped("Audio output selection is only available on Linux with PulseAudio.");
        return;
    }

    if (m_outputSinkNames.empty())
    {
        ImGui::TextWrapped("No audio output sinks found. Make sure PulseAudio is running and audio devices are available.");
        return;
    }

    // Create C-style array for ImGui Combo
    std::vector<const char *> sinkNamesCStr;
    for (const auto &name : m_outputSinkNames)
    {
        sinkNamesCStr.push_back(name.c_str());
    }

    // Add "None" option at the beginning
    std::vector<const char *> sinkNamesWithNone;
    sinkNamesWithNone.push_back("None (Disable Monitoring)");
    for (const auto &name : sinkNamesCStr)
    {
        sinkNamesWithNone.push_back(name);
    }

    // Output sink selection combo
    int selectedIndex = m_monitoringEnabled ? (m_selectedOutputSinkIndex + 1) : 0; // +1 because "None" is at index 0
    if (ImGui::Combo("Output Sink", &selectedIndex, sinkNamesWithNone.data(), static_cast<int>(sinkNamesWithNone.size())))
    {
        if (selectedIndex == 0)
        {
            // "None" selected - disable monitoring
            pulseCapture->removeMonitoringOutput();
            m_monitoringEnabled = false;
            m_selectedOutputSinkIndex = -1;
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.0f, 1.0f), "Monitoring disabled");
        }
        else
        {
            // Output sink selected - enable monitoring
            int sinkIndex = selectedIndex - 1; // -1 because "None" is at index 0
            if (sinkIndex >= 0 && sinkIndex < static_cast<int>(m_outputSinkIds.size()))
            {
                std::string selectedSinkId = m_outputSinkIds[sinkIndex];
                
                if (pulseCapture->setMonitoringOutput(selectedSinkId))
                {
                    m_monitoringEnabled = true;
                    m_selectedOutputSinkIndex = sinkIndex;
                    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Monitoring enabled");
                }
                else
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Failed to enable monitoring");
                    selectedIndex = 0; // Revert to "None"
                    m_monitoringEnabled = false;
                    m_selectedOutputSinkIndex = -1;
                }
            }
        }
    }

    ImGui::Spacing();

    // Current monitoring output info
    std::string currentOutput = pulseCapture->getCurrentMonitoringOutput();
    if (!currentOutput.empty())
    {
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "● Monitoring Active");
        ImGui::Text("Output: %s", currentOutput.c_str());
        ImGui::TextWrapped("You should now hear the captured audio through the selected output sink.");
    }
    else
    {
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.0f, 1.0f), "Monitoring disabled");
        ImGui::TextWrapped("Select an output sink above to hear the captured audio in real-time.");
    }
#else
    ImGui::TextWrapped("Audio output selection is only available on Linux with PulseAudio.");
#endif
}
