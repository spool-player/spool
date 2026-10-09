#include <QCoreApplication>
#include <QFileInfo>
#include <QJSEngine>
#include <QJSValue>
#include <QTimer>
#include <cstdio>

#ifdef SPOOL_TEST_RUNNER
#include "TestMain.h"
#endif

class ContractResult final : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    Q_INVOKABLE void complete(bool success, const QString& reason = {})
    {
        if (settled)
            return;
        settled = true;
        std::fprintf(success ? stdout : stderr, "Qt %s provider contract %s%s%s\n", qVersion(),
            success ? "passed" : "failed", reason.isEmpty() ? "" : ": ", qPrintable(reason));
        QCoreApplication::exit(success ? 0 : 1);
    }

private:
    bool settled = false;
};

#ifdef SPOOL_TEST_RUNNER
SPOOL_TEST_MAIN("bundled-jellyfin")
#else
int main(int argc, char **argv)
#endif
{
    QCoreApplication app(argc, argv);
#ifdef SPOOL_TEST_RUNNER
    if (app.arguments().size() != 1 && app.arguments().size() != 2) {
#else
    if (app.arguments().size() != 2) {
#endif
        std::fprintf(stderr, "usage: provider-contract-runner tests.mjs\n");
        return 2;
    }
#ifdef SPOOL_TEST_RUNNER
    const QString fixture = app.arguments().size() == 1
        ? SpoolTests::fixturePath("tests/providers/fixtures/bundled-jellyfin.mjs")
        : app.arguments()[1];
#else
    const QString fixture = app.arguments()[1];
#endif
    QJSEngine engine;
    ContractResult result;
    QJSEngine::setObjectOwnership(&result, QJSEngine::CppOwnership);
    QTimer::singleShot(0, &app, [&] {
        const QJSValue module = engine.importModule(QFileInfo(fixture).absoluteFilePath());
        if (module.isError() || !module.property(QStringLiteral("run")).isCallable()) {
            std::fprintf(stderr, "Contract module did not load or export run(): %s\n",
                qPrintable(module.isError() ? module.toString() : QStringLiteral("missing run")));
            result.complete(false);
            return;
        }
        QJSValue invoke = engine.evaluate(QStringLiteral(R"JS(
            (function(module, result) {
                try {
                    Promise.resolve(module.run()).then(function() { result.complete(true); },
                        function(error) { result.complete(false, String(error) + "\n" + (error && error.stack || "")); });
                } catch (error) { result.complete(false, String(error) + "\n" + (error && error.stack || "")); }
            })
        )JS"));
        invoke.call({ module, engine.newQObject(&result) });
    });
    QTimer::singleShot(10000, &app, [&] { result.complete(false, QStringLiteral("timed out")); });
    return app.exec();
}

#include "provider-contract-runner.moc"
