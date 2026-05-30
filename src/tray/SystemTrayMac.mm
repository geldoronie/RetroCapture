// macOS system-tray backend — NSStatusItem.
//
// Native AppKit. The status item + its NSMenu live on the main
// thread; menu actions are delivered by the Cocoa run loop, which
// GLFW pumps from glfwPollEvents() each iteration — so the
// TrayMenuItem callbacks fire on the main thread and pump() is a
// no-op.
//
// Compiled only when CMake defines RETROCAPTURE_TRAY_BACKEND_MACOS.

#include "ISystemTray.h"
#include "../utils/Logger.h"
#include "../utils/Paths.h"

#import <AppKit/AppKit.h>

#include <string>
#include <vector>

// Obj-C target that bridges NSMenuItem clicks + status-button clicks
// back to the C++ TrayMenuItem callbacks. One instance per tray.
@interface RCTrayTarget : NSObject
{
@public
    std::vector<retrocapture::TrayMenuItem> *items;
    std::function<void()>                   *onActivate;
}
- (void)menuItemClicked:(id)sender;
- (void)statusItemClicked:(id)sender;
@end

@implementation RCTrayTarget
- (void)menuItemClicked:(id)sender
{
    NSMenuItem *mi = (NSMenuItem *)sender;
    NSInteger idx = [mi tag];
    if (items && idx >= 0 && idx < (NSInteger)items->size())
    {
        const auto &it = (*items)[(size_t)idx];
        if (it.onClick) it.onClick();
    }
}
- (void)statusItemClicked:(id)sender
{
    (void)sender;
    if (onActivate && *onActivate) (*onActivate)();
}
@end

namespace retrocapture {

namespace {

class SystemTrayMac : public ISystemTray
{
public:
    ~SystemTrayMac() override { stop(); }

    bool isSupported() const override { return m_statusItem != nil; }
    bool start(const std::string &iconName, const std::string &tooltip) override;
    void stop() override;

    void setMenu(const std::vector<TrayMenuItem> &items) override
    {
        m_items = items;
        rebuildMenu();
    }
    void updateMenu(const std::vector<TrayMenuItem> &items) override
    {
        m_items = items;
        rebuildMenu();
    }
    void setOnActivate(std::function<void()> cb) override { m_onActivate = std::move(cb); }
    void pump() override {}  // Cocoa run loop (driven by glfwPollEvents) delivers actions.

private:
    void rebuildMenu();

    NSStatusItem *m_statusItem = nil;
    RCTrayTarget *m_target     = nil;
    std::vector<TrayMenuItem> m_items;
    std::function<void()>     m_onActivate;
};

bool SystemTrayMac::start(const std::string &iconName, const std::string &tooltip)
{
    (void)iconName;
    @autoreleasepool {
        m_statusItem = [[NSStatusBar systemStatusBar]
            statusItemWithLength:NSVariableStatusItemLength];
        if (m_statusItem == nil)
        {
            LOG_WARN("tray(mac): failed to create NSStatusItem");
            return false;
        }
        [m_statusItem retain]; // keep alive past the autorelease pool

        m_target = [[RCTrayTarget alloc] init];
        m_target->items      = &m_items;
        m_target->onActivate = &m_onActivate;

        // Branded icon from assets/logo.png, sized for the menu bar.
        std::string iconPath = Paths::getReadOnlyAssetsDir();
        if (!iconPath.empty() && iconPath.back() != '/') iconPath += '/';
        iconPath += "assets/logo.png";
        NSString *p = [NSString stringWithUTF8String:iconPath.c_str()];
        NSImage *img = [[NSImage alloc] initWithContentsOfFile:p];
        if (img == nil)
        {
            // Fallback: resolve via the app bundle's Resources/assets/
            // regardless of CWD / install layout.
            NSString *bp = [[NSBundle mainBundle] pathForResource:@"logo"
                                                           ofType:@"png"
                                                      inDirectory:@"assets"];
            if (bp) img = [[NSImage alloc] initWithContentsOfFile:bp];
        }
        if (img)
        {
            [img setSize:NSMakeSize(18, 18)]; // menu-bar height-ish
            m_statusItem.button.image = img;
            [img release];
        }
        else
        {
            LOG_WARN("tray(mac): couldn't load " + iconPath + " — using text title");
            m_statusItem.button.title = @"RC";
        }
        m_statusItem.button.toolTip = [NSString stringWithUTF8String:tooltip.c_str()];

        // Left-click → activate. We set a button action; right-click /
        // menu is handled by assigning the menu (NSStatusItem shows it
        // on click). To support BOTH left-activate and the context
        // menu, we attach the menu (clicking opens it). The menu's
        // first action effectively replaces a separate activate, which
        // matches platform convention on macOS (status items open their
        // menu on click).
        rebuildMenu();

        LOG_INFO("tray(mac): NSStatusItem created");
        return true;
    }
}

void SystemTrayMac::stop()
{
    @autoreleasepool {
        if (m_statusItem != nil)
        {
            [[NSStatusBar systemStatusBar] removeStatusItem:m_statusItem];
            [m_statusItem release];
            m_statusItem = nil;
        }
        if (m_target != nil)
        {
            [m_target release];
            m_target = nil;
        }
    }
}

void SystemTrayMac::rebuildMenu()
{
    if (m_statusItem == nil) return;
    @autoreleasepool {
        NSMenu *menu = [[NSMenu alloc] init];
        [menu setAutoenablesItems:NO];

        for (size_t i = 0; i < m_items.size(); ++i)
        {
            const TrayMenuItem &it = m_items[i];
            if (it.type == TrayMenuItem::Type::Separator)
            {
                [menu addItem:[NSMenuItem separatorItem]];
                continue;
            }
            NSString *label = [NSString stringWithUTF8String:it.label.c_str()];
            NSMenuItem *mi = [[NSMenuItem alloc]
                initWithTitle:label
                       action:@selector(menuItemClicked:)
                keyEquivalent:@""];
            [mi setTarget:m_target];
            [mi setTag:(NSInteger)i];
            [mi setEnabled:(it.enabled ? YES : NO)];
            if (it.type == TrayMenuItem::Type::Checkbox)
            {
                [mi setState:(it.checked ? NSControlStateValueOn : NSControlStateValueOff)];
            }
            [menu addItem:mi];
            [mi release];
        }

        m_statusItem.menu = menu;
        [menu release];
    }
}

} // namespace

std::unique_ptr<ISystemTray> createSystemTrayMac()
{
    return std::make_unique<SystemTrayMac>();
}

} // namespace retrocapture
