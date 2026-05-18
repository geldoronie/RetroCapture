#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

class UIManager;
class UIRemoteConnection;
class DirectoryBrowser;

/**
 * Stand-alone window that lists the public RetroCapture streams
 * pulled from the directory service. Reachable via the Remote menu
 * ("Browse public directory…"), independent of the manual-URL
 * Connect window so the user can keep browsing while a connection is
 * being set up or torn down.
 *
 * Clicking a row arms the connect path: the URL goes into UIManager's
 * RemoteAuthToken / current device, optionally after the user types
 * the password in the modal that appears for `passwordRequired: true`
 * entries. The actual TCP/HTTP work is dispatched through the
 * existing `UIRemoteConnection::triggerConnect` entry point so the
 * connect/disconnect state machine and the "Connecting…" feedback
 * stay in one place.
 */
class UIDirectoryBrowser
{
public:
    explicit UIDirectoryBrowser(UIManager *uiManager);
    ~UIDirectoryBrowser();

    UIDirectoryBrowser(const UIDirectoryBrowser &)            = delete;
    UIDirectoryBrowser &operator=(const UIDirectoryBrowser &) = delete;

    void render();
    void setVisible(bool visible) { m_visible = visible; }
    bool isVisible() const        { return m_visible; }

    /// Wired by Application — the network fetcher whose snapshot
    /// drives the table. Stays alive across visibility toggles.
    void setDirectoryBrowser(DirectoryBrowser *b) { m_browser = b; }

    /// Wired by UIManager — the connect-state-machine sibling we
    /// hand the selected URL to.
    void setRemoteConnectionWindow(UIRemoteConnection *w) { m_remoteWindow = w; }

private:
    void renderTable();
    void renderPasswordModal();
    void renderReportModal();

    UIManager          *m_uiManager     = nullptr;
    DirectoryBrowser   *m_browser       = nullptr;
    UIRemoteConnection *m_remoteWindow  = nullptr;

    bool m_visible = false;

    // Sort + search state. Kept on the window so the user's choice
    // survives close/reopen within one process run; the ImGui ini
    // doesn't persist this.
    int  m_sortIndex = 0;          // 0=clients, 1=recent, 2=name
    char m_search[64] = {};

    // Password prompt state for protected streams.
    bool         m_showPasswordModal = false;
    char         m_passwordBuffer[128] = {};
    std::string  m_pendingProtectedUrl;

    // Report-this-stream state (#57). When the user right-clicks a
    // row and picks "Report this stream..." the modal opens with
    // these fields. Submission runs on a detached thread and updates
    // m_reportStatus under m_reportMu so the UI thread sees the
    // result on the next frame.
    bool         m_showReportModal = false;
    std::string  m_reportStreamId;
    std::string  m_reportStreamName;     // shown in the modal header
    char         m_reportReason[256]  = {};
    char         m_reportContact[96]  = {};
    enum class ReportStatus
    {
        Idle,        // modal just opened
        Sending,     // POST in flight
        Success,
        Failed,
    };
    mutable std::mutex   m_reportMu;
    ReportStatus         m_reportStatus = ReportStatus::Idle;
    std::string          m_reportError;          // set when Failed
    std::string          m_reportReceiptId;      // set when Success, echoed from /report response
    // Timestamp of the transition into Success state. Used to drive
    // the auto-close countdown so the user reads the receipt without
    // having to dismiss the modal manually.
    std::chrono::steady_clock::time_point m_reportSuccessAt{};
    std::atomic<bool>    m_reportInFlight{false};
};
