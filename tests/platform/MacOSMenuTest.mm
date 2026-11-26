#include "TestMain.h"
#include "TestRequire.h"
#include "app/RouterController.h"
#include "app/SettingsController.h"
#include "cache/DatabaseManager.h"
#include "platform/NativeAppWindow.h"
#include "platform/PlatformApplicationServices.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QList>
#include <QTemporaryDir>

#import <AppKit/AppKit.h>

namespace {

class KeyReceipt final : public QObject {
public:
    QList<int> presses;
    QList<int> releases;

    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease)
            return false;
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->modifiers() != Qt::MetaModifier)
            return false;
        if (event->type() == QEvent::KeyPress)
            presses.append(key->key());
        else
            releases.append(key->key());
        return true;
    }
};

} // namespace

SPOOL_TEST_MAIN("macos-native-menu")
{
    using SpoolTests::require;
    qputenv("QT_QPA_PLATFORM", "cocoa");
    QGuiApplication app(argc, argv);
    QTemporaryDir profile;
    require(profile.isValid(), "create the private menu profile");
    Spool::DatabaseManager database;
    require(
        database.initialize(profile.filePath(QStringLiteral("settings.sqlite"))), "open the menu settings database");
    Spool::SettingsController settings(&database, nullptr, nullptr);
    QCoro::waitFor(settings.loadLocalAsync());
    Spool::NativeAppWindow window(QStringLiteral("native-menu-test"));
    KeyReceipt keys;
    window.installEventFilter(&keys);
    Spool::RouterController router;
    Spool::ApplicationHooks hooks;
    hooks.settings = &settings;
    const QString key = QStringLiteral("appearance/macOSNavigateMenu");
    require(!settings.value(key).toBool(), "a fresh profile leaves the Navigate preference disabled");

    // Let the Cocoa startup hooks finish before taking ownership of the fixture menu.
    app.processEvents();
    NSMenu *original = NSApp.mainMenu;
    NSMenu *testMenu = [[NSMenu alloc] initWithTitle:@"Fixture Menu Bar"];
    NSMenuItem *appItem = [[NSMenuItem alloc] initWithTitle:@"Existing App Menu" action:nullptr keyEquivalent:@""];
    appItem.submenu = [[NSMenu alloc] initWithTitle:appItem.title];
    [testMenu addItem:appItem];
    NSApp.mainMenu = testMenu;
    NSArray<NSMenuItem *> *before = testMenu.itemArray;
    require(NSApp.mainMenu == testMenu && [before containsObject:appItem], "install the Cocoa fixture menu");
    {
        Spool::PlatformApplicationServices services(app, window, hooks, router);
        services.start();
        require([testMenu.itemArray isEqualToArray:before], "starting with the preference off preserves the menu");
        QCoro::waitFor(settings.applyValues({ { key, true } }, Spool::ChangeOrigin::User));
        NSMenuItem *navigate = [testMenu itemWithTitle:@"Navigate"];
        require(navigate && navigate.submenu.numberOfItems == 3, "the opt-in adds a menu with three native commands");
        NSMutableArray<NSMenuItem *> *enabledItems = [before mutableCopy];
        [enabledItems insertObject:navigate atIndex:1];
        require([testMenu.itemArray isEqualToArray:enabledItems], "the opt-in inserts only one item at index one");
        for (NSMenuItem *command in navigate.submenu.itemArray) {
            require(command.keyEquivalent.length == 0, "Qt keeps ownership of the physical keyboard shortcut");
            require(
                [NSApp sendAction:command.action to:command.target from:command], "invoke the native command target");
        }
        QCoreApplication::sendPostedEvents(&window);
        const QList<int> expected = { Qt::Key_O, Qt::Key_F, Qt::Key_Comma };
        require(keys.presses == expected && keys.releases == expected,
            "each command delivers one complete app-wide Qt shortcut gesture");
        QCoro::waitFor(settings.applyValues({ { key, false } }, Spool::ChangeOrigin::User));
        require([testMenu.itemArray isEqualToArray:before], "disable removes only the owned menu");
        QCoro::waitFor(settings.applyValues({ { key, true } }, Spool::ChangeOrigin::User));
        require([testMenu.itemArray isEqualToArray:enabledItems], "re-enabling adds one copy of the native menu");
    }
    require([testMenu.itemArray isEqualToArray:before], "service shutdown detaches the native menu");
    NSApp.mainMenu = original;
    return 0;
}
