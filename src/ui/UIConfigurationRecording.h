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

private:
    UIManager *m_uiManager = nullptr;

    void renderRecordingStatus();
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
};
