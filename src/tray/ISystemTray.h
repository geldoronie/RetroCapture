#pragma once

// Cross-platform system-tray abstraction (#86).
//
// The concrete backends are platform-native (StatusNotifierItem via
// D-Bus on Linux, Shell_NotifyIcon on Windows, NSStatusItem on
// macOS) — GLFW has no tray support. `createSystemTray()` returns
// the right backend for the build, or a no-op stub when the
// platform has no implementation / no tray host, so callers never
// need to #ifdef.
//
// Threading: callbacks fire on the thread that calls pump(). The
// app calls pump() once per main-loop iteration, so menu-item and
// activate callbacks run on the main thread — safe to touch
// Application / UIManager state directly.

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace retrocapture {

// One entry in the tray context menu. `id` is stable across
// updateMenu() calls so a backend that diffs (or that addresses
// items by id over IPC, e.g. DBusMenu) can map old→new.
struct TrayMenuItem
{
    enum class Type
    {
        Action,     // clickable label → onClick
        Separator,  // divider; label/enabled/onClick ignored
        Checkbox    // toggle; `checked` rendered, onClick flips intent
    };

    std::string           id;
    std::string           label;
    Type                  type    = Type::Action;
    bool                  enabled = true;
    bool                  checked = false;
    std::function<void()> onClick;
};

class ISystemTray
{
public:
    virtual ~ISystemTray() = default;

    // True when a tray host is actually available (e.g. a
    // StatusNotifierWatcher is registered on Linux). When false the
    // caller should fall back to keeping the window visible / quitting
    // on close, and surface a one-line warning.
    virtual bool isSupported() const = 0;

    // Create the tray icon. `iconName` is a freedesktop icon-theme
    // name on Linux (with a bundled-pixmap fallback handled by the
    // backend); on Windows/macOS it's resolved to the bundled icon.
    // Returns false if the icon couldn't be created.
    virtual bool start(const std::string &iconName,
                       const std::string &tooltip) = 0;

    // Tear down the icon. Idempotent; also called by the destructor.
    virtual void stop() = 0;

    // Replace the whole menu. Safe to call before start().
    virtual void setMenu(const std::vector<TrayMenuItem> &items) = 0;

    // Refresh labels / enabled / checked in place (e.g. flipping
    // "Start Streaming" ↔ "Stop Streaming" each frame). Backends may
    // implement this as a full rebuild; ids let smarter ones diff.
    virtual void updateMenu(const std::vector<TrayMenuItem> &items) = 0;

    // Primary activation (left-click on most platforms). Optional.
    virtual void setOnActivate(std::function<void()> cb) = 0;

    // Show a desktop notification (e.g. "Streaming started"). Backends
    // route to the platform-native facility: org.freedesktop.Notifications
    // on Linux, the Shell_NotifyIcon balloon on Windows, NSUserNotification
    // on macOS. Callers gate this on the user's "tray notifications"
    // preference; a backend with no notification facility is free to
    // no-op.
    virtual void notify(const std::string &title, const std::string &body) = 0;

    // Drain backend events and invoke pending callbacks on the
    // calling thread. Call once per main-loop iteration. No-op for
    // backends that deliver on their own thread.
    virtual void pump() = 0;
};

// Returns the platform tray backend, or a no-op stub if this build
// has none. Never returns nullptr.
std::unique_ptr<ISystemTray> createSystemTray();

} // namespace retrocapture
