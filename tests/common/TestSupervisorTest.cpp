#include "TestMain.h"
#include "TestRequire.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <vector>
#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_UNIX)
#include <unistd.h>
#endif
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
void startSupervisor(QProcess& process, const QString& path, const QStringList& extra = {}, const QString& tree = {})
{
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("SPOOL_TEST_RESUME"), QStringLiteral("0"));
    environment.insert(QStringLiteral("SPOOL_TEST_RETRY_FAILED"), QStringLiteral("0"));
    environment.insert(QStringLiteral("SPOOL_TEST_FIXTURE_TREE"), tree);
    process.setProcessEnvironment(environment);
    QStringList arguments { QStringLiteral("--run-all"), QStringLiteral("--fixture-suite"), QStringLiteral("--results"),
        path };
    if (!extra.contains(QStringLiteral("--workers")))
        arguments.append({ QStringLiteral("--workers"), QStringLiteral("2") });
    arguments.append(extra);
    process.start(QCoreApplication::applicationFilePath(), arguments);
    require(process.waitForStarted(10000), "start same-binary supervisor");
}
int run(const QString& path, const QStringList& extra = {}, const QString& tree = {})
{
    QProcess process;
    startSupervisor(process, path, extra, tree);
    require(process.waitForFinished(30000), "supervisor terminates after failed and crashed selectors");
    require(process.exitStatus() == QProcess::NormalExit, "selector crash must not crash supervisor");
    return process.exitCode();
}
QString status(const QJsonObject& rows, const QString& name)
{
    return rows.value(name).toObject().value(QStringLiteral("status")).toString();
}
template <typename Predicate> bool waitUntil(Predicate predicate)
{
    QElapsedTimer deadline;
    deadline.start();
    do {
        if (predicate())
            return true;
        QThread::msleep(10);
    } while (deadline.elapsed() < 10000);
    return false;
}
bool alive(qint64 pid)
{
#ifdef Q_OS_WIN
    struct ProcessHandle {
        HANDLE value;
        ~ProcessHandle()
        {
            if (value)
                CloseHandle(value);
        }
    } handle { OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid)) };
    if (!handle.value) {
        require(GetLastError() == ERROR_INVALID_PARAMETER, "inspect descendant OS lifetime");
        return false;
    }
    const DWORD result = WaitForSingleObject(handle.value, 0);
    require(result != WAIT_FAILED, "observe descendant OS termination");
    return result == WAIT_TIMEOUT;
#elif defined(Q_OS_UNIX)
    if (::kill(static_cast<pid_t>(pid), 0) == -1) {
        require(errno == ESRCH || errno == EPERM, "inspect descendant OS lifetime");
        return errno == EPERM;
    }
#ifdef Q_OS_LINUX
    // A killed orphan may await init's reap. A zombie cannot execute or retain
    // resources; don't mistake its remaining PID entry for a live descendant.
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (stat.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = stat.readAll();
        const int commandEnd = bytes.lastIndexOf(')');
        if (commandEnd >= 0 && bytes.mid(commandEnd + 2, 1) == "Z")
            return false;
    }
#endif
    return true;
#else
    return false;
#endif
}
std::vector<qint64> treePids(const QString& root)
{
    std::vector<qint64> pids;
    for (const auto& selector : { "fixture-hang-tree", "fixture-hang-branch", "fixture-hang-leaf" }) {
        QFile file(root + QLatin1Char('.') + QString::fromLatin1(selector));
        require(waitUntil([&] { return file.exists(); }), "actual three-level subprocess tree starts");
        require(file.open(QIODevice::ReadOnly), "read actual descendant PID");
        bool valid = false;
        const qint64 pid = file.readAll().toLongLong(&valid);
        require(valid && pid > 0, "descendant publishes its own OS PID");
        pids.push_back(pid);
    }
    return pids;
}
void requireTreeStopped(const std::vector<qint64>& pids)
{
    require(waitUntil([&] { return std::none_of(pids.begin(), pids.end(), [](qint64 pid) { return alive(pid); }); }),
        "selector, child and grandchild all terminate, not just the direct QProcess");
}
}

SPOOL_TEST_MAIN("test-supervisor")
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "private supervisor regression directory");
    const QString path = temporary.filePath(QStringLiteral("results.json"));
    for (const auto& selector :
        { "fixture-crash", "fixture-access-violation", "fixture-forward-crash", "fixture-exit-three" }) {
        QProcess process;
        process.start(app.applicationFilePath(),
            { QStringLiteral("--fixture-suite"), QStringLiteral("--child"), QString::fromLatin1(selector) });
        require(process.waitForStarted(10000) && process.waitForFinished(10000),
            "real OS fixture exits promptly without dialogs");
        const bool normalThree = QString::fromLatin1(selector) == QStringLiteral("fixture-exit-three");
        require(process.exitStatus() == (normalThree ? QProcess::NormalExit : QProcess::CrashExit),
            "abort and access violation are actual OS crashes; ordinary exit 3 is never inferred as crash");
        if (normalThree)
            require(process.exitCode() == 3, "ordinary exit 3 remains its real failure code");
#ifdef Q_OS_WIN
        else
            require(static_cast<unsigned int>(process.exitCode())
                    == (QString::fromLatin1(selector) == QStringLiteral("fixture-crash") ? 0xC0000025u : 0xC0000005u),
                "Windows retains actual failing NTSTATUS for abort and memory access violation");
#elif defined(Q_OS_UNIX)
        else
            require(process.exitCode()
                    == (QString::fromLatin1(selector) == QStringLiteral("fixture-crash") ? SIGABRT : SIGSEGV),
                "selector retains its actual terminating signal, including a forwarded grandchild crash");
#endif
    }
    require(run(path) == 1, "normal failure and crash aggregate to failure");
    const auto first = journal(path);
    require(run(path, { QStringLiteral("--retry-failed") }) == 2, "retry without resume is rejected");
    require(journal(path) == first, "invalid retry never overwrites the journal or repeats crashed selectors");
    const auto rows = first.value(QStringLiteral("results")).toObject();
    require(rows.size() == 8, "all selectors retain terminal results despite earlier failures");
    require(status(rows, QStringLiteral("fixture-fail")) == QStringLiteral("failed"), "normal failure retained");
    require(status(rows, QStringLiteral("fixture-crash")) == QStringLiteral("crashed"), "OS crash retained");
    require(status(rows, QStringLiteral("fixture-access-violation")) == QStringLiteral("crashed"),
        "real memory fault retained");
    require(status(rows, QStringLiteral("fixture-forward-crash")) == QStringLiteral("crashed"),
        "fast nested subprocess crash propagates as an actual selector crash, not ordinary failure");
    require(status(rows, QStringLiteral("fixture-exit-three")) == QStringLiteral("failed"),
        "normal exit 3 is a failure, not crash heuristic");
    require(status(rows, QStringLiteral("fixture-skip")) == QStringLiteral("skipped"), "skip is not a pass");
    require(status(rows, QStringLiteral("fixture-pass-a")) == QStringLiteral("passed")
            && status(rows, QStringLiteral("fixture-pass-b")) == QStringLiteral("passed"),
        "noncrashing selectors continue and pass");
    require(first.value(QStringLiteral("peakWorkers")).toInt() == 2, "two workers actually execute concurrently");
    struct Event {
        qint64 time;
        int delta;
    };
    std::vector<Event> events;
    for (const auto& value : rows) {
        const auto row = value.toObject();
        events.push_back({ row.value(QStringLiteral("startedMs")).toVariant().toLongLong(), 1 });
        events.push_back({ row.value(QStringLiteral("finishedMs")).toVariant().toLongLong(), -1 });
    }
    std::sort(events.begin(), events.end(),
        [](const Event& a, const Event& b) { return a.time < b.time || (a.time == b.time && a.delta < b.delta); });
    int concurrent = 0;
    for (const auto& event : events) {
        concurrent += event.delta;
        require(concurrent <= 2, "worker bound holds throughout actual selector lifetimes");
    }
    require(run(path, { QStringLiteral("--resume") }) == 1, "resume cannot turn failed/crashed run green");
    const auto resumed = journal(path).value(QStringLiteral("results")).toObject();
    for (const auto& selector : { "fixture-crash", "fixture-access-violation", "fixture-forward-crash" }) {
        const QString name = QString::fromLatin1(selector);
        const auto crashed = resumed.value(name).toObject();
        require(crashed.value(QStringLiteral("resumeSkipped")).toBool()
                && crashed.value(QStringLiteral("attempts")).toArray().size() == 1
                && status(resumed, name) == QStringLiteral("crashed"),
            "resume skips known crashing selectors without marking them passed or rerunning them");
    }
    require(run(path, { QStringLiteral("--resume"), QStringLiteral("--retry-failed") }) == 1,
        "retry failure still reports aggregate failure");
    const auto retried = journal(path).value(QStringLiteral("results")).toObject();
    require(retried.value(QStringLiteral("fixture-fail")).toObject().value(QStringLiteral("attempts")).toArray().size()
            == 2,
        "explicit retry reruns normal failure");
    const auto failures
        = retried.value(QStringLiteral("fixture-fail")).toObject().value(QStringLiteral("attempts")).toArray();
    const QString originalLog = failures[0].toObject().value(QStringLiteral("log")).toString();
    require(!originalLog.isEmpty() && originalLog != failures[1].toObject().value(QStringLiteral("log")).toString(),
        "each retry has its own diagnostic log");
    QFile originalFailure(originalLog);
    require(originalFailure.open(QIODevice::ReadOnly)
            && originalFailure.readAll() == QByteArray("fixture-fail: deliberate failure\n"),
        "retry preserves original failure output as well as its result");
    for (const auto& selector : { "fixture-crash", "fixture-access-violation", "fixture-forward-crash" })
        require(
            retried.value(QString::fromLatin1(selector)).toObject().value(QStringLiteral("attempts")).toArray().size()
                == 1,
            "retry never repeats a known abort, access violation or forwarded crash");
    require(
        retried.value(QStringLiteral("fixture-pass-a")).toObject().value(QStringLiteral("attempts")).toArray().size()
            == 1,
        "resume retains successful selectors without repeating side effects");

    const QString timeoutPath = temporary.filePath(QStringLiteral("timeout.json"));
    const QString timeoutTree = temporary.filePath(QStringLiteral("timeout-tree"));
    const QStringList hang { QStringLiteral("--filter"), QStringLiteral("fixture-hang-tree"),
        QStringLiteral("--workers"), QStringLiteral("1"), QStringLiteral("--timeout-ms"), QStringLiteral("3000") };
    require(run(timeoutPath, hang, timeoutTree) == 1, "actual hung process tree times out as failure");
    require(
        status(journal(timeoutPath).value(QStringLiteral("results")).toObject(), QStringLiteral("fixture-hang-tree"))
            == QStringLiteral("timed-out"),
        "timeout is not a fabricated crash or pass");
    requireTreeStopped(treePids(timeoutTree));

    for (const bool retryInterrupted : { false, true }) {
        const QString interruptedPath = temporary.filePath(
            retryInterrupted ? QStringLiteral("retry-interrupted.json") : QStringLiteral("resume-interrupted.json"));
        const QString interruptedTree = interruptedPath + QStringLiteral("-tree");
        QProcess supervisor;
        startSupervisor(supervisor, interruptedPath,
            { QStringLiteral("--filter"), QStringLiteral("fixture-hang-tree"), QStringLiteral("--workers"),
                QStringLiteral("1") },
            interruptedTree);
        const auto pids = treePids(interruptedTree);
        require(std::all_of(pids.begin(), pids.end(), [](qint64 pid) { return alive(pid); }),
            "all three descendant levels are genuinely running before teardown");
        const auto runningRow = journal(interruptedPath)
                                    .value(QStringLiteral("results"))
                                    .toObject()
                                    .value(QStringLiteral("fixture-hang-tree"))
                                    .toObject();
        require(runningRow.value(QStringLiteral("status")).toString() == QStringLiteral("running"),
            "interrupt at a real committed running-journal boundary");
#ifdef Q_OS_WIN
        supervisor.kill(); // Closing the supervisor's last job handles kills descendants.
#else
        supervisor.terminate(); // SIGTERM asks the owning supervisor to reclaim its groups.
#endif
        require(supervisor.waitForFinished(10000), "interrupted supervisor has bounded teardown");
        requireTreeStopped(pids);
        require(status(journal(interruptedPath).value(QStringLiteral("results")).toObject(),
                    QStringLiteral("fixture-hang-tree"))
                == QStringLiteral("running"),
            "interruption never invents an observed selector crash");
        QFile interruptedEvidence(runningRow.value(QStringLiteral("log")).toString());
        require(interruptedEvidence.open(QIODevice::ReadOnly), "read original interrupted selector evidence");
        const QByteArray originalEvidence = interruptedEvidence.readAll();
        require(!originalEvidence.isEmpty(), "interrupted attempt retains real selector output");
        QStringList resumeArguments = hang;
        resumeArguments.append(QStringLiteral("--resume"));
        if (retryInterrupted)
            resumeArguments.append(QStringLiteral("--retry-failed"));
        const QString resumedTree = interruptedTree + QStringLiteral("-resumed");
        require(run(interruptedPath, resumeArguments, resumedTree) == 1,
            "interrupted selector really runs again and cannot fake a pass");
        requireTreeStopped(treePids(resumedTree));
        const auto row = journal(interruptedPath)
                             .value(QStringLiteral("results"))
                             .toObject()
                             .value(QStringLiteral("fixture-hang-tree"))
                             .toObject();
        const auto attempts = row.value(QStringLiteral("attempts")).toArray();
        require(attempts.size() == 2
                && attempts[0].toObject().value(QStringLiteral("status")).toString() == QStringLiteral("interrupted")
                && attempts[1].toObject().value(QStringLiteral("status")).toString() == QStringLiteral("timed-out")
                && !row.value(QStringLiteral("resumeSkipped")).toBool(),
            "interrupted is distinct from known crash and resumable with or without retry");
        require(attempts[0].toObject().value(QStringLiteral("log")) == runningRow.value(QStringLiteral("log"))
                && attempts[0].toObject().value(QStringLiteral("log"))
                    != attempts[1].toObject().value(QStringLiteral("log"))
                && QFile::exists(attempts[0].toObject().value(QStringLiteral("log")).toString())
                && !attempts[0].toObject().contains(QStringLiteral("exitCode")),
            "interrupted attempt preserves original evidence without inventing an exit code");
        require(interruptedEvidence.seek(0) && interruptedEvidence.readAll() == originalEvidence,
            "resume cannot overwrite the original interrupted attempt's output");
    }
    return 0;
}
