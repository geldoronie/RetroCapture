#pragma once

#include <string>
#include <vector>

#ifdef __APPLE__
#include "../capture/IVideoCapture.h"
#endif

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
    void setVisible(bool visible) { m_visible = visible; }
    bool isVisible() const        { return m_visible; }

private:
    bool m_visible = false;
    UIManager *m_uiManager = nullptr;
    IAudioCapture *m_audioCapture = nullptr;
    
    // Input sources
    std::vector<std::string> m_inputSourceNames;
    std::vector<std::string> m_inputSourceIds;
    int m_selectedInputSourceIndex = -1;
    bool m_inputSourcesListNeedsRefresh = true;
    
    void renderInputSourceSelection();
    void refreshInputSources();
#ifdef __APPLE__
    // AVFoundation device-bundled audio device picker. AVFoundation
    // capture devices (UVC HDMI grabbers) carry their own audio
    // streams; this dropdown lets the user choose between the device's
    // bundled audio and any other AVFoundation audio source on the
    // system. Independent of the IAudioCapture (CoreAudio) path that
    // the encoder/recorder consume.
    void renderAVFoundationAudioDeviceSelection();
    std::vector<DeviceInfo> m_avfAudioDevices;
    bool m_avfAudioDevicesNeedRefresh = true;
#endif
};
