#pragma once

#include "../streaming/CloudflaredDownloader.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
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

    // Cloudflared auto-download UI state (#53 / Phase 2.5b).
    //
    // The download worker runs on a detached thread and updates these
    // fields from there; render reads them under m_cfMu so we don't
    // tear a half-written std::string between the writer and the UI
    // sampler.
    void renderCloudflaredDownload();
    mutable std::mutex             m_cfMu;
    CloudflaredDownloader::Progress m_cfProgress{};
    bool                            m_cfStartedThisRun = false; // suppress modal re-trigger after Ready

    void refreshProfiles();
};
