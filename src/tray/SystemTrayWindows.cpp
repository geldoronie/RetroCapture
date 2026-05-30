// Windows system-tray backend — Shell_NotifyIcon.
//
// Native Win32, no extra libraries beyond shell32/user32 (already
// linked). A hidden message window receives the tray callback
// message; right-click pops a TrackPopupMenu built from the
// TrayMenuItem list, left-click fires onActivate. All servicing
// happens in pump() on the main thread (PeekMessage filtered to our
// own HWND so we don't steal GLFW's messages), so menu/activate
// callbacks run on the main thread.
//
// Compiled only when CMake defines RETROCAPTURE_TRAY_BACKEND_WINDOWS.

#include "ISystemTray.h"
#include "../utils/Logger.h"
#include "../utils/Paths.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace retrocapture {

namespace {

constexpr UINT  kTrayCallbackMsg = WM_APP + 1;
constexpr UINT  kTrayIconId      = 1;
// Menu command ids start here so they don't collide with 0 (cancel).
constexpr UINT  kCmdBase         = 100;
const wchar_t  *kWndClassName    = L"RetroCaptureTrayWindow";

std::wstring utf8ToWide(const std::string &s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

class SystemTrayWindows : public ISystemTray
{
public:
    ~SystemTrayWindows() override { stop(); }

    bool isSupported() const override { return m_added; }
    bool start(const std::string &iconName, const std::string &tooltip) override;
    void stop() override;

    void setMenu(const std::vector<TrayMenuItem> &items) override { m_items = items; }
    void updateMenu(const std::vector<TrayMenuItem> &items) override
    {
        m_items = items;
        // Refresh the tooltip only; the popup menu is rebuilt on demand
        // each right-click so label/enabled changes are picked up live.
    }
    void setOnActivate(std::function<void()> cb) override { m_onActivate = std::move(cb); }
    void notify(const std::string &title, const std::string &body) override;
    void pump() override;

    LRESULT wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    void showContextMenu();

    HWND  m_hwnd  = nullptr;
    HICON m_icon  = nullptr;
    bool  m_added = false;
    UINT  m_taskbarCreatedMsg = 0;   // re-add icon if explorer restarts
    std::string m_tooltip;

    std::vector<TrayMenuItem> m_items;
    std::function<void()>     m_onActivate;
};

LRESULT CALLBACK trayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto *self = reinterpret_cast<SystemTrayWindows *>(
        GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    if (self)
    {
        return self->wndProc(hWnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool SystemTrayWindows::start(const std::string &iconName, const std::string &tooltip)
{
    (void)iconName;
    m_tooltip = tooltip;

    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = trayWndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = kWndClassName;
    RegisterClassExW(&wc); // ignore "already registered"

    // Normal hidden window (not message-only) so TrackPopupMenu can
    // foreground it. Never shown.
    m_hwnd = CreateWindowExW(0, kWndClassName, L"RetroCaptureTray",
                             WS_OVERLAPPED, 0, 0, 0, 0,
                             nullptr, nullptr, hInst, nullptr);
    if (!m_hwnd)
    {
        LOG_WARN("tray(win): CreateWindow failed");
        return false;
    }
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    m_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    // Load our branded icon from assets/logo.ico; fall back to the
    // stock application icon.
    std::string icoPath = Paths::getReadOnlyAssetsDir();
    if (!icoPath.empty() && icoPath.back() != '/' && icoPath.back() != '\\')
        icoPath += '/';
    icoPath += "assets/logo.ico";
    std::wstring icoW = utf8ToWide(icoPath);
    m_icon = (HICON)LoadImageW(nullptr, icoW.c_str(), IMAGE_ICON, 0, 0,
                               LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if (!m_icon)
    {
        LOG_WARN("tray(win): couldn't load " + icoPath + " — using default icon");
        // IDI_APPLICATION is MAKEINTRESOURCE (an ordinal packed into a
        // pointer); the A-macro yields char* which LoadIconW rejects.
        // The W variant interprets the low word as the ordinal, so the
        // reinterpret_cast is safe.
        m_icon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    }

    NOTIFYICONDATAW nid = {};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = m_hwnd;
    nid.uID              = kTrayIconId;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayCallbackMsg;
    nid.hIcon            = m_icon;
    std::wstring tipW = utf8ToWide(m_tooltip);
    lstrcpynW(nid.szTip, tipW.c_str(), (int)(sizeof(nid.szTip) / sizeof(wchar_t)));

    if (!Shell_NotifyIconW(NIM_ADD, &nid))
    {
        LOG_WARN("tray(win): Shell_NotifyIcon NIM_ADD failed");
        return false;
    }
    m_added = true;
    LOG_INFO("tray(win): notification icon added");
    return true;
}

void SystemTrayWindows::stop()
{
    if (m_added && m_hwnd)
    {
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd   = m_hwnd;
        nid.uID    = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        m_added = false;
    }
    if (m_hwnd)
    {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_icon)
    {
        DestroyIcon(m_icon);
        m_icon = nullptr;
    }
}

void SystemTrayWindows::notify(const std::string &title, const std::string &body)
{
    if (!m_added || !m_hwnd) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize      = sizeof(nid);
    nid.hWnd        = m_hwnd;
    nid.uID         = kTrayIconId;
    nid.uFlags      = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    std::wstring titleW = utf8ToWide(title);
    std::wstring bodyW  = utf8ToWide(body);
    lstrcpynW(nid.szInfoTitle, titleW.c_str(),
              (int)(sizeof(nid.szInfoTitle) / sizeof(wchar_t)));
    lstrcpynW(nid.szInfo, bodyW.c_str(),
              (int)(sizeof(nid.szInfo) / sizeof(wchar_t)));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void SystemTrayWindows::pump()
{
    if (!m_hwnd) return;
    MSG msg;
    // Filter to our own window so we don't drain GLFW's queue.
    while (PeekMessageW(&msg, m_hwnd, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void SystemTrayWindows::showContextMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    for (size_t i = 0; i < m_items.size(); ++i)
    {
        const TrayMenuItem &it = m_items[i];
        if (it.type == TrayMenuItem::Type::Separator)
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        UINT flags = MF_STRING;
        if (!it.enabled) flags |= MF_GRAYED;
        if (it.type == TrayMenuItem::Type::Checkbox && it.checked) flags |= MF_CHECKED;
        AppendMenuW(menu, flags, kCmdBase + (UINT)i, utf8ToWide(it.label).c_str());
    }

    // Standard tray-menu foreground dance so the menu dismisses when
    // the user clicks elsewhere.
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(m_hwnd);
    UINT cmd = TrackPopupMenu(menu,
                              TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                              pt.x, pt.y, 0, m_hwnd, nullptr);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd >= kCmdBase)
    {
        size_t idx = cmd - kCmdBase;
        if (idx < m_items.size() && m_items[idx].onClick)
        {
            m_items[idx].onClick();
        }
    }
}

LRESULT SystemTrayWindows::wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == kTrayCallbackMsg)
    {
        switch (LOWORD(lParam))
        {
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
                if (m_onActivate) m_onActivate();
                break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                showContextMenu();
                break;
            default:
                break;
        }
        return 0;
    }
    if (m_taskbarCreatedMsg != 0 && msg == m_taskbarCreatedMsg)
    {
        // Explorer restarted — re-add the icon.
        if (m_hwnd && m_icon)
        {
            NOTIFYICONDATAW nid = {};
            nid.cbSize           = sizeof(nid);
            nid.hWnd             = m_hwnd;
            nid.uID              = kTrayIconId;
            nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            nid.uCallbackMessage = kTrayCallbackMsg;
            nid.hIcon            = m_icon;
            std::wstring tipW = utf8ToWide(m_tooltip);
            lstrcpynW(nid.szTip, tipW.c_str(), (int)(sizeof(nid.szTip) / sizeof(wchar_t)));
            Shell_NotifyIconW(NIM_ADD, &nid);
        }
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace

std::unique_ptr<ISystemTray> createSystemTrayWindows()
{
    return std::make_unique<SystemTrayWindows>();
}

} // namespace retrocapture
