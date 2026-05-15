#pragma once

#include <string>

class UIManager;
class IVideoCapture;
class DirectoryBrowser;

/**
 * UIRemoteConnection — dedicated window for the Remote viewer mode.
 *
 * Two tabs:
 *   - Manual URL: type a remote base URL and click Connect (original
 *     flow).
 *   - Browse directory (#49 Phase 4): live list of streams pulled from
 *     the directory service; click a row to populate the Manual URL
 *     tab and connect.
 *
 * Common to both: display interpolation dropdown and connection
 * status / stream dimensions.
 *
 * Lives outside the Source tab on purpose: connecting to a remote
 * RetroCapture instance is conceptually a different operating mode
 * ("client viewer") rather than a capture-source choice, so it sits
 * under its own top-level menu.
 */
class UIRemoteConnection
{
public:
    UIRemoteConnection(UIManager *uiManager);
    ~UIRemoteConnection();

    void render();
    void setVisible(bool visible);
    bool isVisible() const { return m_visible; }

    // setCapture is intentionally a no-op kept for ABI compatibility
    // with the wiring in Application. The window was crashing on
    // use-after-free because the m_capture pointer it cached went
    // dangling the moment the user clicked Connect (the device-change
    // callback destroys and recreates m_capture inside the same frame).
    // Now we read all connection state through UIManager getters which
    // remain valid across the swap.
    void setCapture(IVideoCapture *) {}

    /// Inject the directory browser. Application owns the lifetime;
    /// this window just reads snapshots and triggers refreshes.
    void setDirectoryBrowser(DirectoryBrowser *b) { m_browser = b; }

    /**
     * Arm the connect state machine with the given URL. Called from
     * UIDirectoryBrowser when the user picks a stream from the list,
     * so the two-frame 'Connecting…' feedback and the existing
     * triggerSourceTypeChange / setCurrentDevice plumbing live in
     * exactly one place. Also opens this window so the user can see
     * the progress.
     */
    void triggerConnect(const std::string &url);

private:
    UIManager *m_uiManager = nullptr;
    bool m_visible = false;

    DirectoryBrowser *m_browser = nullptr;

    // ImGui InputText buffer, seeded from the saved device path on
    // first render so the user's previous URL persists across sessions.
    char m_urlBuffer[256] = {};
    bool m_urlSeeded = false;

    // Two-frame state machine for connect/disconnect feedback. The
    // setCurrentDevice() path blocks ~50-100 ms; if we executed it on
    // the same frame as the button click, the user never sees a
    // status change — the click and the result render together. By
    // deferring the actual call to the NEXT frame we get one paint of
    // 'Connecting...' / 'Disconnecting...' on screen before the
    // blocking call starts.
    enum class PendingAction
    {
        None,
        ConnectShowStatus,   // show 'Connecting...' this frame
        ConnectExecute,      // run the blocking call this frame
        DisconnectShowStatus,
        DisconnectExecute,
    };
    PendingAction m_pending = PendingAction::None;
    std::string m_pendingUrl;

    void renderManualTab(bool sourceIsRemote, const std::string &currentDevice, bool connected);
    void renderStatusFooter(bool connected);
    void advanceStateMachine();
};
