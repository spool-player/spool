#include "platform/PlatformApplicationServices.h"

#include "app/SettingsController.h"
#include "platform/NativeAppWindow.h"
#include "platform/apple/AppleSystemMediaControls.h"
#include "player/PlayerController.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPointer>

#import <AppKit/AppKit.h>

@interface SpoolMenuActions : NSObject {
@public
    QPointer<Spool::NativeAppWindow> window;
}
- (void)invokeShortcut:(NSMenuItem *)item;
@end

@implementation SpoolMenuActions
- (void)invokeShortcut:(NSMenuItem *)item
{
    if (!window || QGuiApplication::modalWindow())
        return;
    const int key = static_cast<int>(item.tag);
    // The shell already owns shortcut admission, navigation and file picking.
    // Its Control+F branch opens the Settings filter. Meta reaches app search.
    // Queue the gesture after AppKit has closed its menu tracking loop.
    constexpr auto modifier = Qt::MetaModifier;
    QCoreApplication::postEvent(window, new QKeyEvent(QEvent::KeyPress, key, modifier));
    QCoreApplication::postEvent(window, new QKeyEvent(QEvent::KeyRelease, key, modifier));
}
@end

namespace Spool {

struct PlatformApplicationServices::PlatformData : QObject {
    QGuiApplication& application;
    NativeAppWindow& window;
    ApplicationHooks& hooks;
    std::unique_ptr<AppleSystemMediaControls> mediaControls;
    SpoolMenuActions *actions = nil;
    NSMenuItem *navigateItem = nil;
    __weak NSMenu *attachedMenu = nil;

    PlatformData(QGuiApplication& app, NativeAppWindow& window, ApplicationHooks& hooks)
        : application(app)
        , window(window)
        , hooks(hooks)
    {
    }

    ~PlatformData() override
    {
        detach();
    }

    void detach()
    {
        NSMenu *owner = attachedMenu;
        if (owner && [owner indexOfItem:navigateItem] >= 0)
            [owner removeItem:navigateItem];
        attachedMenu = nil;
    }

    void syncMenu()
    {
        const bool enabled = hooks.settings->value(QStringLiteral("appearance/macOSNavigateMenu")).toBool();
        NSMenu *owner = enabled ? NSApp.mainMenu : nil;
        if (owner.numberOfItems == 0)
            owner = nil;
        if (owner == attachedMenu && (!owner || [owner indexOfItem:navigateItem] >= 0))
            return;
        detach();
        if (!owner)
            return;
        if (!navigateItem) {
            actions = [[SpoolMenuActions alloc] init];
            actions->window = &window;
            navigateItem = [[NSMenuItem alloc] initWithTitle:@"Navigate" action:nullptr keyEquivalent:@""];
            navigateItem.submenu = [[NSMenu alloc] initWithTitle:navigateItem.title];
            addAction(@"Open Files…", Qt::Key_O);
            addAction(@"Search", Qt::Key_F);
            addAction(@"Settings…", Qt::Key_Comma);
        }
        [owner insertItem:navigateItem atIndex:1];
        attachedMenu = owner;
    }

    void addAction(NSString *title, Qt::Key key)
    {
        // Physical keyboard shortcuts are still handled by the QML shell.
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:@selector(invokeShortcut:) keyEquivalent:@""];
        item.target = actions;
        item.tag = key;
        [navigateItem.submenu addItem:item];
    }
};

PlatformApplicationServices::PlatformApplicationServices(
    QGuiApplication& application, NativeAppWindow& window, ApplicationHooks& hooks, RouterController&)
    : m_platform(std::make_unique<PlatformData>(application, window, hooks))
{
}

PlatformApplicationServices::~PlatformApplicationServices() = default;

void PlatformApplicationServices::start()
{
    auto *data = m_platform.get();
    if (!data->hooks.settings || QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return;
    const auto syncMenu = [data] { data->syncMenu(); };
    QObject::connect(data->hooks.settings, &SettingsController::settingsValuesChanged, data, syncMenu);
    QObject::connect(&data->application, &QGuiApplication::focusWindowChanged, data, syncMenu);
    QObject::connect(&data->window, &NativeAppWindow::platformSurfaceExposed, data, syncMenu);
    syncMenu();

    if (auto *player = data->hooks.player) {
        data->mediaControls = std::make_unique<AppleSystemMediaControls>(data->hooks);
        QObject::connect(player, &PlayerController::sessionActiveChanged, data->mediaControls.get(),
            &AppleSystemMediaControls::updateNowPlaying);
    }
}

} // namespace Spool
