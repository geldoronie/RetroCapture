#pragma once

#include <string>
#include <cstdint>
#include <vector>

// Forward declarations
class UIManager;

/**
 * Recording tab in configuration window
 */
class UIConfigurationRecording
{
public:
    UIConfigurationRecording(UIManager *uiManager);
    ~UIConfigurationRecording();

    void render();
    void setVisible(bool visible) { m_visible = visible; }
    bool isVisible() const        { return m_visible; }

private:
    bool m_visible = false;
    UIManager *m_uiManager = nullptr;

    void renderRecordingStatus();
    void renderProfiles();
    void renderBasicSettings();
    void renderCodecSettings();
    void renderBitrateSettings();
    void renderContainerSettings();
    void renderOutputSettings();
    void renderStartStopButton();

    // Codec-specific settings
    void renderH264Settings();
    void renderH265Settings();
    void renderVP8Settings();
    void renderVP9Settings();

    // Profile UI state
    int m_selectedProfileIndex = -1;
    std::vector<std::string> m_profileNames;
    bool m_profilesDirty = true;
    char m_newProfileName[128] = "";
    bool m_showSaveDialog = false;
    bool m_showDeleteConfirm = false;
    bool m_showOverwriteConfirm = false;

    void refreshProfiles();
};
