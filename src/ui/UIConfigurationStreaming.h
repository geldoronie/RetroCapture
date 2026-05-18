#pragma once

#include "../streaming/CloudflaredAccount.h"
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

    // Cloudflared Named-tunnel UI state (#60 / Phase 2.5c).
    //
    // Three async operations share this block: login (long-running
    // OAuth wait), tunnel-list refresh (10s `cloudflared tunnel list`),
    // and route-dns / create (10–30s each). All run on detached
    // threads and update fields under m_cfNamedMu; render samples
    // under the same lock.
    void renderCloudflaredNamedTunnelSetup();

    mutable std::mutex                          m_cfNamedMu;
    CloudflaredAccount::LoginProgress           m_loginProgress{};
    bool                                        m_loginStartedThisRun = false;
    std::vector<CloudflaredAccount::TunnelInfo> m_namedTunnels;
    std::string                                 m_namedTunnelsError;
    bool                                        m_namedTunnelsLoaded   = false;
    std::atomic<bool>                           m_namedTunnelsRefreshing{false};
    // Create-new-tunnel modal state
    bool                                        m_showCreateTunnelModal = false;
    char                                        m_newTunnelName[64]     = {};
    std::string                                 m_createTunnelError;
    std::atomic<bool>                           m_createTunnelInFlight{false};
    // DNS route result + busy flag
    std::string                                 m_lastRouteResult;
    bool                                        m_lastRouteOk          = false;
    std::atomic<bool>                           m_routeInFlight{false};

    void refreshProfiles();
};
