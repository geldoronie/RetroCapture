#pragma once

#include <string>
#include <vector>

// Forward declarations
class UIManager;
class IAudioCapture;
class IVideoCapture;

/**
 * Audio configuration tab in the configuration window
 */
class UIConfigurationAudio
{
public:
    UIConfigurationAudio(UIManager *uiManager);
    ~UIConfigurationAudio();

    void render();

private:
    UIManager *m_uiManager = nullptr;
    IAudioCapture *m_audioCapture = nullptr;
    IVideoCapture *m_capture = nullptr;
    
    // Input sources (Linux PulseAudio)
    std::vector<std::string> m_inputSourceNames;
    std::vector<std::string> m_inputSourceIds;
    int m_selectedInputSourceIndex = -1;
    bool m_inputSourcesListNeedsRefresh = true;
    
    void renderInputSourceSelection();
    void refreshInputSources();
    
#ifdef __APPLE__
    void renderAVFoundationAudioDeviceSelection();
#endif
};
