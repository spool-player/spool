#include <QCoreApplication>
#include <QFileInfo>
#include <QJSEngine>
#include <QJSValue>
#include <QTimer>
#include <cstdio>

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

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 2) {
        std::fprintf(stderr, "usage: provider-contract-runner tests.mjs\n");
        return 2;
    }
    QJSEngine engine;
    ContractResult result;
    QJSEngine::setObjectOwnership(&result, QJSEngine::CppOwnership);
    QTimer::singleShot(0, &app, [&] {
        const QJSValue module = engine.importModule(QFileInfo(app.arguments()[1]).absoluteFilePath());
        if (module.isError() || !module.property(QStringLiteral("run")).isCallable()) {
            std::fprintf(stderr, "Contract module did not load or export run()\n");
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
