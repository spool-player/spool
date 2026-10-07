#include "TestMain.h"
#include "TestRequire.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <algorithm>
#include <vector>
using SpoolTests::require;

namespace {
QJsonObject journal(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "supervisor retains its result journal");
    const auto document = QJsonDocument::fromJson(file.readAll());
    require(document.isObject(), "supervisor journal is valid JSON");
    return document.object();
}
int run(const QString& path, const QStringList& extra = {})
{
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("SPOOL_TEST_RESUME"), QStringLiteral("0"));
    environment.insert(QStringLiteral("SPOOL_TEST_RETRY_FAILED"), QStringLiteral("0"));
    process.setProcessEnvironment(environment);
    QStringList arguments { QStringLiteral("--run-all"), QStringLiteral("--fixture-suite"),
        QStringLiteral("--workers"), QStringLiteral("2"), QStringLiteral("--results"), path };
    arguments.append(extra);
    process.start(QCoreApplication::applicationFilePath(), arguments);
    require(process.waitForStarted(10000), "start same-binary supervisor");
    require(process.waitForFinished(30000), "supervisor terminates after failed and crashed selectors");
    require(process.exitStatus() == QProcess::NormalExit, "selector crash must not crash supervisor");
    return process.exitCode();
}
QString status(const QJsonObject& rows, const QString& name)
{
    return rows.value(name).toObject().value(QStringLiteral("status")).toString();
}
}

SPOOL_TEST_MAIN("test-supervisor")
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "private supervisor regression directory");
    const QString path = temporary.filePath(QStringLiteral("results.json"));
    require(run(path) == 1, "normal failure and crash aggregate to failure");
    const auto first = journal(path);
    require(run(path, { QStringLiteral("--retry-failed") }) == 2, "retry without resume is rejected");
    require(journal(path) == first, "invalid retry never overwrites the journal or repeats crashed selectors");
    const auto rows = first.value(QStringLiteral("results")).toObject();
    require(rows.size() == 5, "all selectors retain terminal results despite earlier failures");
    require(status(rows, QStringLiteral("fixture-fail")) == QStringLiteral("failed"), "normal failure retained");
    require(status(rows, QStringLiteral("fixture-crash")) == QStringLiteral("crashed"), "OS crash retained");
    require(status(rows, QStringLiteral("fixture-skip")) == QStringLiteral("skipped"), "skip is not a pass");
    require(status(rows, QStringLiteral("fixture-pass-a")) == QStringLiteral("passed")
            && status(rows, QStringLiteral("fixture-pass-b")) == QStringLiteral("passed"),
        "noncrashing selectors continue and pass");
    require(first.value(QStringLiteral("peakWorkers")).toInt() == 2, "two workers actually execute concurrently");
    struct Event { qint64 time; int delta; };
    std::vector<Event> events;
    for (const auto& value : rows) {
        const auto row = value.toObject();
        events.push_back({ row.value(QStringLiteral("startedMs")).toVariant().toLongLong(), 1 });
        events.push_back({ row.value(QStringLiteral("finishedMs")).toVariant().toLongLong(), -1 });
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return a.time < b.time || (a.time == b.time && a.delta < b.delta);
    });
    int concurrent = 0;
    for (const auto& event : events) {
        concurrent += event.delta;
        require(concurrent <= 2, "worker bound holds throughout actual selector lifetimes");
    }
    require(run(path, { QStringLiteral("--resume") }) == 1, "resume cannot turn failed/crashed run green");
    const auto resumed = journal(path).value(QStringLiteral("results")).toObject();
    const auto crashed = resumed.value(QStringLiteral("fixture-crash")).toObject();
    require(crashed.value(QStringLiteral("resumeSkipped")).toBool()
            && crashed.value(QStringLiteral("attempts")).toArray().size() == 1
            && status(resumed, QStringLiteral("fixture-crash")) == QStringLiteral("crashed"),
        "resume skips known crashing selector without marking it passed or rerunning it");
    require(run(path, { QStringLiteral("--resume"), QStringLiteral("--retry-failed") }) == 1,
        "retry failure still reports aggregate failure");
    const auto retried = journal(path).value(QStringLiteral("results")).toObject();
    require(retried.value(QStringLiteral("fixture-fail")).toObject().value(QStringLiteral("attempts")).toArray().size() == 2,
        "explicit retry reruns normal failure");
    const auto failures = retried.value(QStringLiteral("fixture-fail")).toObject().value(QStringLiteral("attempts")).toArray();
    const QString originalLog = failures[0].toObject().value(QStringLiteral("log")).toString();
    require(!originalLog.isEmpty() && originalLog != failures[1].toObject().value(QStringLiteral("log")).toString(),
        "each retry has its own diagnostic log");
    QFile originalFailure(originalLog);
    require(originalFailure.open(QIODevice::ReadOnly)
            && originalFailure.readAll() == QByteArray("fixture-fail: deliberate failure\n"),
        "retry preserves original failure output as well as its result");
    require(retried.value(QStringLiteral("fixture-crash")).toObject().value(QStringLiteral("attempts")).toArray().size() == 1,
        "retry never repeats known crash");
    require(retried.value(QStringLiteral("fixture-pass-a")).toObject().value(QStringLiteral("attempts")).toArray().size() == 1,
        "resume retains successful selectors without repeating side effects");
    return 0;
}
