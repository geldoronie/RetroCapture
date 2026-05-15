#pragma once

#include <string>
#include <cstdint>
#include <vector>

// Forward declarations
class UIManager;

/**
 * Aba Streaming da janela de configuração
 */
class UIConfigurationStreaming
{
public:
    UIConfigurationStreaming(UIManager *uiManager);
    ~UIConfigurationStreaming();

    void render();

private:
    UIManager *m_uiManager = nullptr;

    void renderStreamingStatus();
    void renderProfiles();
    void renderBasicSettings();
    void renderCodecSettings();
    void renderBitrateSettings();
    void renderAdvancedBufferSettings();
    void renderStartStopButton();
    void renderDirectoryPublish();        // #49 Phase 2

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

    // Directory-publish UI state (#49 Phase 2)
    bool m_dirShowPrivacyModal = false;       // true while privacy popup is open

    void refreshProfiles();
};
