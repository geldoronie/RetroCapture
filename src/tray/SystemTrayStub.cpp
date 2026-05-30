// No-op tray backend + the createSystemTray() factory.
//
// The factory returns a platform backend when one is compiled in
// (selected by the build via RETROCAPTURE_TRAY_BACKEND_* defines),
// otherwise this stub. The stub reports isSupported() == false so
// the app cleanly falls back to keep-window / quit-on-close.

#include "ISystemTray.h"

namespace retrocapture {

namespace {

class SystemTrayStub : public ISystemTray
{
public:
    bool isSupported() const override { return false; }
    bool start(const std::string &, const std::string &) override { return false; }
    void stop() override {}
    void setMenu(const std::vector<TrayMenuItem> &) override {}
    void updateMenu(const std::vector<TrayMenuItem> &) override {}
    void setOnActivate(std::function<void()>) override {}
    void notify(const std::string &, const std::string &) override {}
    void pump() override {}
};

} // namespace

// Platform backends provide their own factory symbol; the build wires
// exactly one of these in. When none is compiled, we fall back to the
// stub.
#if defined(RETROCAPTURE_TRAY_BACKEND_LINUX)
std::unique_ptr<ISystemTray> createSystemTrayLinux();
#endif
#if defined(RETROCAPTURE_TRAY_BACKEND_WINDOWS)
std::unique_ptr<ISystemTray> createSystemTrayWindows();
#endif
#if defined(RETROCAPTURE_TRAY_BACKEND_MACOS)
std::unique_ptr<ISystemTray> createSystemTrayMac();
#endif

std::unique_ptr<ISystemTray> createSystemTray()
{
#if defined(RETROCAPTURE_TRAY_BACKEND_LINUX)
    if (auto tray = createSystemTrayLinux())   return tray;
#endif
#if defined(RETROCAPTURE_TRAY_BACKEND_WINDOWS)
    if (auto tray = createSystemTrayWindows()) return tray;
#endif
#if defined(RETROCAPTURE_TRAY_BACKEND_MACOS)
    if (auto tray = createSystemTrayMac())     return tray;
#endif
    return std::make_unique<SystemTrayStub>();
}

} // namespace retrocapture
