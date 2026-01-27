#include "UIConfigurationAudio.h"
#include "UIManager.h"
#include "../audio/IAudioCapture.h"
#include "../capture/IVideoCapture.h"
#include "../utils/Logger.h"
#ifdef __linux__
#include "../audio/AudioCapturePulse.h"
#endif
#ifdef __APPLE__
// Forward declaration - VideoCaptureAVFoundation is Objective-C++ and cannot be included in C++ file
// Use virtual methods from IVideoCapture interface instead
class VideoCaptureAVFoundation;
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
    m_capture = m_uiManager->getCapture();

    // Audio monitoring sample rate configuration
    ImGui::Text("Audio Monitoring Configuration");
    ImGui::Separator();
    
    // Get current audio monitoring sample rate from capture device
    uint32_t detectedSampleRate = 0;
    if (m_capture && m_capture->hasAudio() && m_capture->isOpen())
    {
        detectedSampleRate = m_capture->getAudioSampleRate();
    }
    
    // Get manual sample rate setting from UIManager
    uint32_t manualSampleRate = m_uiManager->getAudioMonitoringSampleRate();
    uint32_t currentSampleRate = (manualSampleRate > 0) ? manualSampleRate : detectedSampleRate;
    
    // Sample rate selection dropdown
    const char* sampleRateOptions[] = {
        "Auto (from device)", "8000 Hz", "11025 Hz", "16000 Hz", 
        "22050 Hz", "44100 Hz", "48000 Hz", "96000 Hz"
    };
    uint32_t sampleRateValues[] = {0, 8000, 11025, 16000, 22050, 44100, 48000, 96000};
    int selectedIndex = 0; // Default to Auto
    
    // Find current selection
    if (currentSampleRate > 0)
    {
        for (int i = 1; i < 8; ++i)
        {
            if (sampleRateValues[i] == currentSampleRate)
            {
                selectedIndex = i;
                break;
            }
        }
        // If not found in list, show "Auto" but keep manual setting
    }
    
    if (ImGui::Combo("Audio Monitoring Sample Rate", &selectedIndex, sampleRateOptions, 8))
    {
        uint32_t newSampleRate = sampleRateValues[selectedIndex];
        m_uiManager->setAudioMonitoringSampleRate(newSampleRate);
        m_uiManager->saveConfig();
        
        // If device is open, trigger reconfiguration
        if (m_capture && m_capture->isOpen() && m_capture->hasAudio())
        {
            // The Application will detect the change and reconfigure audio output
            LOG_INFO("Audio monitoring sample rate changed to: " + 
                     (newSampleRate > 0 ? std::to_string(newSampleRate) + " Hz" : "Auto (from device)"));
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Sample rate for audio monitoring/playback. If 'Auto' is selected, the sample rate will be detected from the capture device. If detection fails, select a manual sample rate.");
    }
    
    // Show detected sample rate
    if (detectedSampleRate > 0)
    {
        ImGui::Text("Detected sample rate: %u Hz", detectedSampleRate);
    }
    else
    {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "No sample rate detected from device");
    }
    
    ImGui::Separator();
    ImGui::Spacing();

#ifdef __APPLE__
    // On macOS, show AVFoundation audio device selection if using AVFoundation
    // Check if capture has audio capability (AVFoundation-specific)
    // We can check by seeing if listAudioDevices() returns non-empty (AVFoundation-specific method)
    if (m_capture && m_capture->isOpen())
    {
        // Try to get audio devices - if this works, it's AVFoundation
        auto audioDevices = m_capture->listAudioDevices();
        if (!audioDevices.empty() || m_uiManager->getAVFoundationAudioDevices().size() > 0)
        {
            renderAVFoundationAudioDeviceSelection();
            ImGui::Separator();
        }
    }
#endif

#ifdef __linux__
    // On Linux, show PulseAudio input source selection
    if (!m_audioCapture)
    {
        ImGui::TextWrapped("Audio capture not available. Audio is required for streaming and recording.");
        return;
    }
    renderInputSourceSelection();
#else
    // On macOS without AVFoundation audio, show message
    if (!m_capture || !m_capture->isOpen())
    {
        ImGui::TextWrapped("No video capture device open. Open a device in the Source tab to configure audio monitoring.");
    }
#endif
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
                // Save configuration
                if (m_uiManager)
                {
                    m_uiManager->setAudioInputSourceId(selectedSourceId);
                    m_uiManager->saveConfig();
                }
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
            // Save configuration (clear saved source)
            if (m_uiManager)
            {
                m_uiManager->setAudioInputSourceId("");
                m_uiManager->saveConfig();
            }
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

#ifdef __APPLE__
void UIConfigurationAudio::renderAVFoundationAudioDeviceSelection()
{
    // Audio device selection for AVFoundation monitoring
    ImGui::Text("AVFoundation Audio Device (for monitoring):");
    ImGui::Separator();
    
    // Obter o ponteiro do capture do UIManager (sempre atualizar)
    m_capture = m_uiManager->getCapture();
    
    // Usar cache de dispositivos de áudio do UIManager
    auto currentAudioDevices = m_uiManager->getAVFoundationAudioDevices();
    
    // Apenas atualizar a lista se estiver vazia E m_capture estiver disponível
    if (currentAudioDevices.empty() && m_capture)
    {
        m_uiManager->refreshAVFoundationAudioDevices();
        currentAudioDevices = m_uiManager->getAVFoundationAudioDevices();
    }
    
    // Combo box for audio device selection
    std::string currentAudioDevice = m_uiManager->getAVFoundationAudioDevice();
    std::string displayText = currentAudioDevice.empty() ? "Auto-detect (None)" : currentAudioDevice;
    
    // Se não houver dispositivos, mostrar mensagem mas ainda permitir seleção de "None"
    if (currentAudioDevices.empty())
    {
        ImGui::TextWrapped("Nenhum dispositivo de áudio AVFoundation encontrado. Clique em Refresh para atualizar.");
        ImGui::Spacing();
    }
    int selectedIndex = -1;
    
    // Verificar se "Auto-detect" está selecionado
    if (currentAudioDevice.empty())
    {
        selectedIndex = 0;
    }
    else
    {
        // Procurar dispositivo na lista (índice +1 porque "Auto-detect" é 0)
        for (size_t i = 0; i < currentAudioDevices.size(); ++i)
        {
            if (currentAudioDevices[i].id == currentAudioDevice || currentAudioDevices[i].name == currentAudioDevice)
            {
                selectedIndex = static_cast<int>(i) + 1; // +1 porque "Auto-detect" é 0
                displayText = currentAudioDevices[i].name + " (" + currentAudioDevices[i].id + ")";
                break;
            }
        }
    }
    
    if (ImGui::BeginCombo("##avfaudiodevice", displayText.c_str()))
    {
        // Opção "Auto-detect" sempre como primeira opção
        bool isAutoSelected = currentAudioDevice.empty();
        if (ImGui::Selectable("Auto-detect (None)", isAutoSelected))
        {
            m_uiManager->setAVFoundationAudioDevice("");
            m_uiManager->saveConfig();
        }
        if (isAutoSelected)
        {
            ImGui::SetItemDefaultFocus();
        }
        
        // Listar dispositivos de áudio disponíveis
        for (size_t i = 0; i < currentAudioDevices.size(); ++i)
        {
            bool isSelected = (selectedIndex == static_cast<int>(i) + 1);
            std::string deviceLabel = currentAudioDevices[i].name + " (" + currentAudioDevices[i].id + ")";
            if (ImGui::Selectable(deviceLabel.c_str(), isSelected))
            {
                // Usar o ID do dispositivo (uniqueID do AVFoundation)
                m_uiManager->setAVFoundationAudioDevice(currentAudioDevices[i].id);
                m_uiManager->saveConfig();
            }
            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    
    ImGui::SameLine();
    if (ImGui::Button("Refresh##avfaudiodevices"))
    {
        m_uiManager->refreshAVFoundationAudioDevices();
    }
    
    ImGui::TextWrapped("Select an audio device to monitor. If 'Auto-detect' is selected, the system will try to find a matching audio device for the selected video device.");
}
#endif
