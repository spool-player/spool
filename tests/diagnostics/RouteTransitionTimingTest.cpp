#include "TestMain.h"

#include <QCoreApplication>
#include <QFile>
#include <QJSEngine>

#include <cstdlib>
#include <iostream>

namespace {
QString read(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        std::cerr << "cannot read route timing fixture\n";
        std::exit(1);
    }
    return QString::fromUtf8(file.readAll());
}
}

JELLYFIN_TEST_MAIN("route-transition-timing")
{
    QCoreApplication app(argc, argv);
    const QString route = read(QStringLiteral(TEST_SOURCE_DIR "/qml/shell/RouteStack.qml"));
    const qsizetype begin = route.indexOf(QStringLiteral("    function showRoute() {"));
    const qsizetype end = route.indexOf(QStringLiteral("    function activatePending() {"), begin);
    if (begin < 0 || end <= begin) {
        std::cerr << "route timing test cannot locate the actual route functions\n";
        return 1;
    }
    QJSEngine engine;
    const QString fixture = read(QStringLiteral(TEST_SOURCE_DIR "/tests/diagnostics/fixtures/route-transition-timing.js"));
    const QJSValue result = engine.evaluate(fixture + '\n' + route.mid(begin, end - begin)
        + QStringLiteral("\nrunRouteTransitionTests();"));
    if (result.isError() || result.toInt() != 5) {
        std::cerr << result.toString().toStdString() << '\n';
        return 1;
    }
    return 0;
}
