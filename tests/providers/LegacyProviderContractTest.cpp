#include "TestMain.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJSEngine>
#include <QJSValue>
#include <QThread>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const QString& message)
{
    if (!condition) {
        std::cerr << qPrintable(message) << '\n';
        std::exit(1);
    }
}
}

SPOOL_TEST_MAIN("provider-legacy-contract")
{
    QCoreApplication app(argc, argv);
    QJSEngine engine;
    const QString gluePath = QStringLiteral(TEST_SOURCE_DIR "/tests/providers/fixtures/api02-95591f09/runtime-glue.js");
    QFile fixture(gluePath);
    require(fixture.open(QIODevice::ReadOnly), QStringLiteral("Cannot open frozen API-0.2 glue"));
    const QJSValue glue = engine.evaluate(QString::fromUtf8(fixture.readAll()), gluePath);
    require(!glue.isError() && glue.property(QStringLiteral("create")).isCallable()
            && glue.property(QStringLiteral("call")).isCallable(),
        QStringLiteral("Frozen API-0.2 glue failed to load: ") + glue.toString());
    const QJSValue contract
        = engine.importModule(QStringLiteral(TEST_SOURCE_DIR "/tests/providers/fixtures/legacy-provider-contract.mjs"));
    require(!contract.isError() && contract.property(QStringLiteral("run")).isCallable(),
        QStringLiteral("First-party legacy contract failed to load: ") + contract.toString() + QLatin1Char('\n')
            + contract.property(QStringLiteral("stack")).toString());
    QJSValue result = engine.newObject();
    const QJSValue invoke = engine.evaluate(QStringLiteral(R"JS(
        (function(contract, glue, result) {
            try {
                Promise.resolve(contract.run(glue)).then(function() {
                    result.success = true;
                    result.done = true;
                }, function(error) {
                    result.error = String(error) + '\n' + (error && error.stack || '');
                    result.done = true;
                });
            } catch (error) {
                result.error = String(error) + '\n' + (error && error.stack || '');
                result.done = true;
            }
        })
    )JS"));
    const QJSValue started = invoke.call({ contract, glue, result });
    require(!started.isError(), started.toString());
    QElapsedTimer deadline;
    deadline.start();
    while (!result.property(QStringLiteral("done")).toBool() && deadline.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(result.property(QStringLiteral("done")).toBool(), QStringLiteral("Legacy provider contract timed out"));
    require(result.property(QStringLiteral("success")).toBool(), result.property(QStringLiteral("error")).toString());
    std::cout << "Frozen API-0.2 provider contracts passed for Jellyfin, Emby and Plex\n";
    return 0;
}
