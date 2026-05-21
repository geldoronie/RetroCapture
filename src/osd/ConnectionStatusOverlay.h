#pragma once

#include <string>

class UIManager;
class QuickActionsOverlay;

/**
 * Remote-stream connection status OSD.
 *
 * When the desktop is in client mode (Source = Remote, currentDevice
 * non-empty), this overlay renders a small spinning label at the
 * bottom-right of the viewport while we transition between states:
 *
 *   Connecting…   — armed device, no frames yet, no prior session
 *   Reconnecting… — armed device, no frames now, had frames before
 *   Disconnecting… — device just cleared, short tail
 *   Connected     — flashed for ~3 s on the first frame
 *   Host appears offline — extended reconnect failure
 *
 * Extracted from UIManager::renderConnectionOverlay during the OSD
 * layering pass (#68). Reads the live state from UIManager through
 * a small set of public getters; owns the transition-tracking state
 * itself so UIManager doesn't have to.
 *
 * Stacks above QuickActionsOverlay when both are visible — queries
 * the widget's renderedHeight() and offsets its anchor upward.
 */
class ConnectionStatusOverlay
{
public:
    ConnectionStatusOverlay(UIManager *uiManager,
                            QuickActionsOverlay *quickActions);
    ~ConnectionStatusOverlay();

    void render();

private:
    UIManager           *m_uiManager    = nullptr;
    QuickActionsOverlay *m_quickActions = nullptr;

    // Per-frame transition tracking. Lives here instead of on
    // UIManager so the overlay is self-contained.
    std::string m_lastDevice;
    bool        m_lastHadFrames        = false;
    double      m_connectedSince       = 0.0;
    double      m_disconnectingUntil   = 0.0;
};
