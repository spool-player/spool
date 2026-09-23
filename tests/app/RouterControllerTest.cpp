#include "app/RouterController.h"

#include "TestMain.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

} // namespace

SPOOL_TEST_MAIN("router-controller")
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("spool-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("router-controller"));
    QTemporaryDir settingsDir;
    require(settingsDir.isValid(), "temporary settings directory");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings().clear();
    Spool::RouterController router;

    require(router.route() == QStringLiteral("login"), "initial route is login");
    router.reset(QStringLiteral("home"));
    require(router.route() == QStringLiteral("home"), "reset route");
    require(!router.canPop(), "reset clears stack");

    router.push(QStringLiteral("libraryGrid"), { { QStringLiteral("libraryId"), QStringLiteral("abc") } });
    require(router.route() == QStringLiteral("libraryGrid"), "push route");
    require(router.previousRoute() == QStringLiteral("home"), "push previous route");
    require(router.canPop(), "push creates stack frame");
    require(router.args().value(QStringLiteral("libraryId")).toString() == QStringLiteral("abc"), "push args");

    router.replace(QStringLiteral("search"));
    require(router.route() == QStringLiteral("search"), "replace route");
    require(router.previousRoute() == QStringLiteral("libraryGrid"), "replace previous route");
    require(router.canPop(), "replace keeps stack");

    router.push(QStringLiteral("itemDetails"), { { QStringLiteral("itemId"), QStringLiteral("m1") } });
    require(router.args().value(QStringLiteral("itemId")).toString() == QStringLiteral("m1"), "detail args");
    require(router.previousRoute() == QStringLiteral("search"), "detail previous route");
    router.replace(QStringLiteral("itemDetails"),
        { { QStringLiteral("itemId"), QStringLiteral("m2") },
            { QStringLiteral("returnRoute"), QStringLiteral("search") } });
    require(router.route() == QStringLiteral("itemDetails"), "detail replace route");
    require(router.args().value(QStringLiteral("itemId")).toString() == QStringLiteral("m2"), "detail replace item");
    require(router.args().value(QStringLiteral("returnRoute")).toString() == QStringLiteral("search"),
        "detail return route");
    require(router.route() == QStringLiteral("itemDetails"), "detail replace stays on details");
    require(router.previousRoute() == QStringLiteral("itemDetails"), "detail replace previous route");

    require(router.pop(QStringLiteral("home")), "pop returns stacked frame");
    require(router.route() == QStringLiteral("search"), "pop restores previous frame");
    require(router.canPop(), "first pop leaves original frame");
    require(router.pop(QStringLiteral("home")), "second pop returns stacked frame");
    require(router.route() == QStringLiteral("home"), "second pop restores home");
    require(!router.canPop(), "pop consumes stack");

    require(!router.pop(QStringLiteral("settings")), "empty pop uses fallback");
    require(router.route() == QStringLiteral("settings"), "empty pop fallback route");

    router.reset(QStringLiteral("home"));
    router.push(QStringLiteral("libraryGrid"),
        { { QStringLiteral("libraryId"), QStringLiteral("round-trip") }, { QStringLiteral("focusIndex"), 42 } });
    require(router.pop(), "back creates forward history");
    require(router.canForward(), "back enables forward navigation");
    require(router.forward(), "forward restores popped frame");
    require(router.route() == QStringLiteral("libraryGrid"), "forward restores route");
    require(router.args().value(QStringLiteral("libraryId")).toString() == QStringLiteral("round-trip"),
        "forward restores route arguments");
    require(router.args().value(QStringLiteral("focusIndex")).toInt() == 42, "forward restores focus argument");
    require(!router.canForward(), "forward consumes history");

    require(router.pop(), "second back creates invalidation candidate");
    router.push(QStringLiteral("search"));
    require(!router.canForward(), "new push invalidates forward history");
    require(!router.forward(), "empty forward history is inert");

    require(router.pop(), "back before replace invalidation");
    router.replace(QStringLiteral("settings"));
    require(!router.canForward(), "replace invalidates forward history");

    router.push(QStringLiteral("search"));
    require(router.pop(), "back before reset invalidation");
    router.reset(QStringLiteral("home"));
    require(!router.canForward(), "reset invalidates forward history");

    {
        Spool::RouterController activeSession;
        activeSession.beginSession(false);
        activeSession.reset(QStringLiteral("home"));
        activeSession.push(
            QStringLiteral("libraryGrid"), { { QStringLiteral("libraryId"), QStringLiteral("library-1") } });
        activeSession.checkpoint(
            { { QStringLiteral("focusIndex"), 742 }, { QStringLiteral("model"), QStringLiteral("must-not-persist") } });
        activeSession.requestRecoveryOnNextLaunch(QStringLiteral("memoryReclaim"));
        activeSession.markCleanShutdown();
    }

    Spool::RouterController recoveredSession;
    recoveredSession.beginSession(true);
    require(recoveredSession.recoveryPending(), "requested session recovery is pending");
    require(recoveredSession.route() == QStringLiteral("libraryGrid"), "recovered route");
    require(recoveredSession.args().value(QStringLiteral("libraryId")).toString() == QStringLiteral("library-1"),
        "recovered library id");
    require(recoveredSession.args().value(QStringLiteral("focusIndex")).toInt() == 742, "recovered grid index");
    require(!recoveredSession.args().contains(QStringLiteral("model")), "runtime model is not persisted");
    require(recoveredSession.canPop(), "recovered navigation stack");
    recoveredSession.finishRecovery();
    require(!recoveredSession.recoveryPending(), "recovery acknowledgement clears pending state");
    recoveredSession.markCleanShutdown();

    return 0;
}
