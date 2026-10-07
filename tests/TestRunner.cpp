#include "TestMain.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QThread>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#ifdef SPOOL_MOBILE_TEST_BUNDLE
#include "platform/MobileTestFixtures.h"
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace SpoolTests {
namespace {
std::map<std::string, Entry>& registry()
{
    static std::map<std::string, Entry> tests;
    return tests;
}
} // namespace
bool registerTest(const char *name, Entry entry)
{
    if (!registry().emplace(name, entry).second)
        std::abort();
    return true;
}
int invoke(const char *name, int argc, char **argv)
{
    const auto selected = registry().find(name);
    return selected == registry().end() ? 2 : selected->second(argc, argv);
}
} // namespace SpoolTests

namespace {
constexpr int skipExitCode = 77;

// These selectors are only available to the runner's own subprocess regression.
// They exercise OS exit/crash semantics rather than mocking QProcess results.
int fixture(const QString& name)
{
    if (name == QStringLiteral("fixture-crash"))
        std::abort();
    QThread::msleep(120);
    if (name == QStringLiteral("fixture-fail")) {
        std::cerr << "fixture-fail: deliberate failure\n";
        return 1;
    }
    if (name == QStringLiteral("fixture-skip"))
        return skipExitCode;
    return name == QStringLiteral("fixture-pass-a") || name == QStringLiteral("fixture-pass-b") ? 0 : 2;
}

bool save(const QString& path, const QJsonObject& state)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Indented);
    return file.write(bytes) == bytes.size() && file.commit();
}

QString receiptPath;
QJsonObject receipt;
void finishReceipt()
{
    if (!receiptPath.isEmpty() && !save(receiptPath, receipt))
        std::fprintf(stderr, "cannot write native test receipt\n");
}

bool initializeReceipt(int argc, char **argv)
{
    QString nonce;
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view(argv[index]) == "--receipt")
            receiptPath = QString::fromUtf8(argv[index + 1]);
        if (std::string_view(argv[index]) == "--nonce")
            nonce = QString::fromUtf8(argv[index + 1]);
    }
    if (receiptPath.isEmpty())
        return true;
    if (!QDir::isAbsolutePath(receiptPath) || nonce.isEmpty() || nonce.size() > 128)
        return false;
    receipt = { { QStringLiteral("format"), 1 }, { QStringLiteral("nonce"), nonce },
        { QStringLiteral("status"), QStringLiteral("failed") }, { QStringLiteral("exitCode"), 1 } };
    std::atexit(finishReceipt);
    return true;
}

int supervise(QCoreApplication& app)
{
    const QStringList arguments = app.arguments();
    const auto option = [&arguments](const QString& name, const QString& fallback) {
        const int index = arguments.indexOf(name);
        return index >= 0 && index + 1 < arguments.size() ? arguments[index + 1] : fallback;
    };
    bool validWorkers = false;
    const int workers = option(QStringLiteral("--workers"), qEnvironmentVariable("SPOOL_TEST_WORKERS", "4"))
                            .toInt(&validWorkers);
    bool validTimeout = false;
    const int timeout = option(QStringLiteral("--timeout-ms"), QStringLiteral("180000")).toInt(&validTimeout);
    if (!validWorkers || workers < 1 || workers > 32 || !validTimeout || timeout < 1) {
        std::cerr << "workers must be 1..32 and timeout-ms must be positive\n";
        return 2;
    }
    const QString path = QFileInfo(option(QStringLiteral("--results"),
        QDir::current().filePath(QStringLiteral("test-results.json")))).absoluteFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return 2;
    const bool resume = arguments.contains(QStringLiteral("--resume"))
        || qEnvironmentVariableIntValue("SPOOL_TEST_RESUME") != 0;
    const bool retry = arguments.contains(QStringLiteral("--retry-failed"))
        || qEnvironmentVariableIntValue("SPOOL_TEST_RETRY_FAILED") != 0;
    if (retry && !resume) {
        std::cerr << "--retry-failed requires --resume; the previous journal must remain intact\n";
        return 2;
    }
    const bool fixtures = arguments.contains(QStringLiteral("--fixture-suite"));
    QStringList names;
    if (fixtures) {
        names = { QStringLiteral("fixture-pass-a"), QStringLiteral("fixture-fail"), QStringLiteral("fixture-crash"),
            QStringLiteral("fixture-pass-b"), QStringLiteral("fixture-skip") };
    } else {
        for (const auto& [name, entry] : SpoolTests::registry())
            names.append(QString::fromStdString(name));
    }
    const QString filter = option(QStringLiteral("--filter"), {});
    if (!filter.isEmpty()) {
        const QStringList requested = filter.split(QLatin1Char(','));
        for (const auto& name : requested) {
            if (!names.contains(name)) {
                std::cerr << "unknown selector in --filter\n";
                return 2;
            }
        }
        names = requested;
        names.removeDuplicates();
    }
    QJsonObject state;
    if (resume && QFileInfo::exists(path)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return 2;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()
            || document.object().value(QStringLiteral("format")).toInt() != 1
            || document.object().value(QStringLiteral("binary")).toString() != app.applicationFilePath()) {
            std::cerr << "result journal is invalid or belongs to another binary\n";
            return 2;
        }
        state = document.object();
    } else {
        state = { { QStringLiteral("format"), 1 }, { QStringLiteral("binary"), app.applicationFilePath() },
            { QStringLiteral("results"), QJsonObject {} } };
    }
    QJsonObject results = state.value(QStringLiteral("results")).toObject();
    QStringList pending;
    for (const auto& name : names) {
        QJsonObject row = results.value(name).toObject();
        QString status = row.value(QStringLiteral("status")).toString();
        if (status == QStringLiteral("running")) {
            status = QStringLiteral("crashed");
            row.insert(QStringLiteral("status"), status);
            row.insert(QStringLiteral("reason"), QStringLiteral("supervisor interrupted while selector was running"));
        }
        if (resume && status == QStringLiteral("crashed")) {
            row.insert(QStringLiteral("resumeSkipped"), true);
        } else if (!resume || status.isEmpty() || status == QStringLiteral("pending")
            || (retry && (status == QStringLiteral("failed") || status == QStringLiteral("timed-out")
                || status == QStringLiteral("start-failed")))) {
            row.insert(QStringLiteral("status"), QStringLiteral("pending"));
            pending.append(name);
        }
        results.insert(name, row);
    }
    state.insert(QStringLiteral("results"), results);
    if (!save(path, state))
        return 2;

    struct Running {
        QString name;
        std::unique_ptr<QProcess> process;
        qint64 started;
        QElapsedTimer elapsed;
    };
    std::vector<Running> running;
    running.reserve(size_t(workers));
    int next = 0;
    int peakWorkers = 0;
    bool journalFailed = false;
    const auto persist = [&] {
        state.insert(QStringLiteral("results"), results);
        state.insert(QStringLiteral("peakWorkers"), peakWorkers);
        if (!save(path, state))
            journalFailed = true;
    };
    while (next < pending.size() || !running.empty()) {
        while (!journalFailed && next < pending.size() && int(running.size()) < workers) {
            const QString name = pending[next++];
            auto process = std::make_unique<QProcess>();
            const int attempt = results.value(name).toObject().value(QStringLiteral("attempts")).toArray().size() + 1;
            const QString logPath = path + QLatin1Char('.') + name + QLatin1Char('.')
                + QString::number(attempt) + QStringLiteral(".log");
            QFile logFile(logPath);
            if (!logFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
                || !logFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
                journalFailed = true;
                break;
            }
            logFile.close();
            process->setProcessChannelMode(QProcess::MergedChannels);
            process->setStandardOutputFile(logPath, QIODevice::Truncate);
            const qint64 started = QDateTime::currentMSecsSinceEpoch();
            QJsonObject row = results.value(name).toObject();
            row.insert(QStringLiteral("status"), QStringLiteral("running"));
            row.insert(QStringLiteral("startedMs"), started);
            row.insert(QStringLiteral("log"), logPath);
            results.insert(name, row);
            persist();
            if (journalFailed)
                break;
            QStringList childArguments { QStringLiteral("--child"), name };
            if (fixtures)
                childArguments.prepend(QStringLiteral("--fixture-suite"));
            process->start(app.applicationFilePath(), childArguments);
            if (!process->waitForStarted(10000)) {
                row.insert(QStringLiteral("status"), QStringLiteral("start-failed"));
                row.insert(QStringLiteral("finishedMs"), QDateTime::currentMSecsSinceEpoch());
                QJsonArray attempts = row.value(QStringLiteral("attempts")).toArray();
                attempts.append(QJsonObject { { QStringLiteral("status"), QStringLiteral("start-failed") },
                    { QStringLiteral("startedMs"), started }, { QStringLiteral("finishedMs"), row.value(QStringLiteral("finishedMs")) },
                    { QStringLiteral("log"), logPath }, { QStringLiteral("exitCode"), -1 } });
                row.insert(QStringLiteral("attempts"), attempts);
                results.insert(name, row);
                persist();
                continue;
            }
            QElapsedTimer elapsed;
            elapsed.start();
            running.push_back({ name, std::move(process), started, elapsed });
            peakWorkers = std::max(peakWorkers, int(running.size()));
        }
        for (auto it = running.begin(); it != running.end();) {
            QProcess& process = *it->process;
            process.waitForFinished(0);
            const bool timedOut = process.state() != QProcess::NotRunning && it->elapsed.elapsed() > timeout;
            if (timedOut || journalFailed) {
                process.kill();
                process.waitForFinished(10000);
            } else {
                process.waitForFinished(5);
            }
            if (process.state() != QProcess::NotRunning) {
                ++it;
                continue;
            }
            const QString status = timedOut ? QStringLiteral("timed-out")
                : process.exitStatus() == QProcess::CrashExit ? QStringLiteral("crashed")
                : process.exitCode() == 0 ? QStringLiteral("passed")
                : process.exitCode() == skipExitCode ? QStringLiteral("skipped") : QStringLiteral("failed");
            QJsonObject row = results.value(it->name).toObject();
            row.insert(QStringLiteral("status"), status);
            row.insert(QStringLiteral("exitCode"), process.exitCode());
            row.insert(QStringLiteral("finishedMs"), QDateTime::currentMSecsSinceEpoch());
            QJsonArray attempts = row.value(QStringLiteral("attempts")).toArray();
            attempts.append(QJsonObject { { QStringLiteral("status"), status },
                { QStringLiteral("startedMs"), it->started },
                { QStringLiteral("finishedMs"), row.value(QStringLiteral("finishedMs")) },
                { QStringLiteral("exitCode"), process.exitCode() }, { QStringLiteral("log"), row.value(QStringLiteral("log")) } });
            row.insert(QStringLiteral("attempts"), attempts);
            results.insert(it->name, row);
            std::cout << it->name.toStdString() << ": " << status.toStdString() << '\n';
            persist();
            it = running.erase(it);
        }
        if (journalFailed) {
            next = pending.size();
        }
        QCoreApplication::processEvents();
    }
    bool failed = journalFailed;
    for (const auto& name : names) {
        const QString status = results.value(name).toObject().value(QStringLiteral("status")).toString();
        failed |= status != QStringLiteral("passed") && status != QStringLiteral("skipped");
    }
    if (journalFailed)
        std::cerr << "cannot retain supervisor journal; remaining selectors were not run\n";
    return failed ? 1 : 0;
}
} // namespace

int main(int argc, char **argv)
{
#ifdef Q_OS_WIN
    // A crash must become an OS exit result, not an interactive WER dialog
    // that blocks the selector indefinitely on an unattended runner.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#ifdef _MSC_VER
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#endif
    if (!initializeReceipt(argc, argv))
        return 2;
#ifdef SPOOL_MOBILE_TEST_BUNDLE
    if (!SpoolTests::prepareMobileFixtures())
        return 2;
#endif
    // Construct QCoreApplication ONLY in supervisor/list mode. The isolated
    // selector owns its one Q(Core/Gui)Application and its complete environment.
    int selectorIndex = 1;
    bool fixtures = false;
    for (int index = 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--fixture-suite")
            fixtures = true;
        if (std::string_view(argv[index]) == "--child")
            selectorIndex = index + 1;
    }
    bool supervisor = argc == 1;
    bool list = false;
    for (int index = 1; index < argc; ++index) {
        supervisor |= std::string_view(argv[index]) == "--run-all";
        list |= std::string_view(argv[index]) == "--list";
    }
    if (supervisor || list) {
        QCoreApplication app(argc, argv);
        if (list) {
            QJsonArray selectors;
            for (const auto& [name, entry] : SpoolTests::registry()) {
                std::cout << name << '\n';
                selectors.append(QString::fromStdString(name));
            }
            receipt.insert(QStringLiteral("selectors"), selectors);
            receipt.insert(QStringLiteral("status"), QStringLiteral("passed"));
            receipt.insert(QStringLiteral("exitCode"), 0);
            return 0;
        }
        return supervise(app);
    }
    if (selectorIndex >= argc)
        return 2;
    if (fixtures)
        return fixture(QString::fromUtf8(argv[selectorIndex]));
    const auto& tests = SpoolTests::registry();
    const auto selected = tests.find(argv[selectorIndex]);
    if (selected == tests.end()) {
        std::cerr << "unknown test selector; use --list\n";
        return 2;
    }
    if (!receiptPath.isEmpty()) {
        receipt.insert(QStringLiteral("selector"), QString::fromStdString(selected->first));
        QJsonObject started = receipt;
        started.insert(QStringLiteral("status"), QStringLiteral("running"));
        if (!save(receiptPath, started))
            return 2;
    }
    std::vector<char *> childArguments;
    childArguments.reserve(size_t(argc - selectorIndex + 2));
    childArguments.push_back(argv[0]);
    for (int index = selectorIndex + 1; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--receipt" || std::string_view(argv[index]) == "--nonce") {
            ++index;
            continue;
        }
        childArguments.push_back(argv[index]);
    }
    QByteArray credentialNonce;
    if (selected->first == "tvos-credentials" && childArguments.size() == 1) {
        credentialNonce = receipt.value(QStringLiteral("nonce")).toString().toUtf8();
        childArguments.push_back(credentialNonce.data());
    }
    const int childCount = int(childArguments.size());
    childArguments.push_back(nullptr);
    const int result = selected->second(childCount, childArguments.data());
    if (!receiptPath.isEmpty()) {
        receipt.insert(QStringLiteral("exitCode"), result);
        receipt.insert(QStringLiteral("status"), result == 0 ? QStringLiteral("passed")
            : result == skipExitCode ? QStringLiteral("skipped") : QStringLiteral("failed"));
    }
    return result;
}
