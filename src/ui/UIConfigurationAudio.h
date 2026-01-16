#pragma once

#include <string>
#include <vector>

// Forward declarations
class UIManager;
class IAudioCapture;

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
    
    // Input sources
    std::vector<std::string> m_inputSourceNames;
    std::vector<std::string> m_inputSourceIds;
    int m_selectedInputSourceIndex = -1;
    bool m_inputSourcesListNeedsRefresh = true;
    
    void renderInputSourceSelection();
    void refreshInputSources();
};
