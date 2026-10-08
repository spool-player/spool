#include "TestMain.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QThread>
#include <algorithm>
#include <cerrno>
#include <csignal>
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
#ifdef Q_OS_UNIX
#include <sys/mman.h>
#include <unistd.h>
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
[[noreturn]] void propagateCrash(int exitCode)
{
#ifdef Q_OS_WIN
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exitCode));
    std::_Exit(1); // Only reachable if the OS rejected termination.
#else
    std::signal(exitCode, SIG_DFL);
    std::raise(exitCode);
    std::abort();
#endif
}
} // namespace SpoolTests

namespace {
constexpr int skipExitCode = 77;

#ifdef Q_OS_WIN
void abortAsOsCrash(int)
{
    // STATUS_FATAL_APP_EXIT is informational and Qt treats it as a normal exit.
    // STATUS_NONCONTINUABLE_EXCEPTION is an actual failing NTSTATUS instead.
    SpoolTests::propagateCrash(static_cast<int>(0xC0000025u));
}
#endif

#ifdef Q_OS_UNIX
volatile std::sig_atomic_t interruptedSignal = 0;
void interruptSupervisor(int signal)
{
    interruptedSignal = signal;
}
#endif

// The owner outlives QProcess and every descendant, including on early returns.
// Windows assigns the job atomically at CreateProcess, before any child code can
// spawn an unowned grandchild. Unix descendants inherit the selector's session.
class SelectorProcess final : public QProcess {
public:
    SelectorProcess()
    {
        connect(this, &QProcess::started, this, [this] { treePid = processId(); });
#ifdef Q_OS_WIN
        job.value = CreateJobObjectW(nullptr, nullptr);
        if (!job.value)
            return;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            return;
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        attributes.reset(new unsigned char[bytes]);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.get());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) {
            startup.lpAttributeList = nullptr;
            return;
        }
        if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &job.value,
                sizeof(job.value), nullptr, nullptr))
            return;
        setCreateProcessArgumentsModifier([this](CreateProcessArguments *arguments) {
            startup.StartupInfo = *arguments->startupInfo;
            startup.StartupInfo.cb = sizeof(startup);
            arguments->startupInfo = &startup.StartupInfo;
            arguments->flags |= EXTENDED_STARTUPINFO_PRESENT;
        });
        ready = true;
#elif defined(Q_OS_UNIX)
        setChildProcessModifier([this] {
            if (::setsid() == -1)
                failChildProcessModifier("setsid", errno);
        });
#endif
    }
    ~SelectorProcess() override
    {
        if (!stopTree())
            std::fprintf(stderr, "cannot terminate selector process tree\n");
        if (state() != NotRunning)
            waitForFinished(10000);
#ifdef Q_OS_WIN
        if (startup.lpAttributeList)
            DeleteProcThreadAttributeList(startup.lpAttributeList);
#endif
    }
    bool ownershipReady() const
    {
#ifdef Q_OS_WIN
        return ready;
#else
        return true;
#endif
    }
    bool stopTree()
    {
        if (!treePid)
            return true;
#ifdef Q_OS_WIN
        if (!TerminateJobObject(job.value, 1))
            return false;
#elif defined(Q_OS_UNIX)
        if (::kill(-static_cast<pid_t>(treePid), SIGKILL) == -1 && errno != ESRCH)
            return false;
#else
        kill();
#endif
        treePid = 0;
        return true;
    }

private:
    qint64 treePid = 0;
#ifdef Q_OS_WIN
    struct JobHandle {
        HANDLE value = nullptr;
        JobHandle() = default;
        JobHandle(const JobHandle&) = delete;
        JobHandle& operator=(const JobHandle&) = delete;
        ~JobHandle()
        {
            if (value)
                CloseHandle(value);
        }
    } job;
    STARTUPINFOEXW startup {};
    std::unique_ptr<unsigned char[]> attributes;
    bool ready = false;
#endif
};

// These selectors are only available to the runner's own subprocess regression.
// They exercise OS exit/crash semantics rather than mocking QProcess results.
int fixture(const QString& name, int argc, char **argv)
{
    if (name == QStringLiteral("fixture-crash"))
        std::abort();
    if (name == QStringLiteral("fixture-access-violation")) {
#ifdef Q_OS_WIN
        void *page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
        if (!page)
            return 2;
        *static_cast<volatile unsigned char *>(page) = 1;
#else
        std::signal(SIGSEGV, SIG_DFL);
        void *page = ::mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED)
            return 2;
        *static_cast<volatile unsigned char *>(page) = 1;
#endif
        return 2;
    }
    if (name == QStringLiteral("fixture-forward-crash")) {
        QCoreApplication app(argc, argv);
        QProcess child;
        child.start(app.applicationFilePath(),
            { QStringLiteral("--fixture-suite"), QStringLiteral("--child"),
                QStringLiteral("fixture-access-violation") });
        if (!child.waitForStarted(10000) || !child.waitForFinished(10000))
            return 2;
        if (child.exitStatus() == QProcess::CrashExit)
            SpoolTests::propagateCrash(child.exitCode());
        return 1;
    }
    if (name == QStringLiteral("fixture-exit-three"))
        return 3;
    if (name == QStringLiteral("fixture-hang-tree") || name == QStringLiteral("fixture-hang-branch")
        || name == QStringLiteral("fixture-hang-leaf")) {
        if (argc < 2)
            return 2;
        QCoreApplication app(argc, argv);
        const QString root = qEnvironmentVariable("SPOOL_TEST_FIXTURE_TREE");
        if (root.isEmpty())
            return 2;
        QProcess child;
        if (name != QStringLiteral("fixture-hang-leaf")) {
            const QString descendant = name == QStringLiteral("fixture-hang-tree")
                ? QStringLiteral("fixture-hang-branch")
                : QStringLiteral("fixture-hang-leaf");
            child.start(app.applicationFilePath(),
                { QStringLiteral("--fixture-suite"), QStringLiteral("--child"), descendant });
            if (!child.waitForStarted(10000))
                return 2;
        }
        std::cout << name.toStdString() << ": deliberate hang\n" << std::flush;
        QSaveFile pidFile(root + QLatin1Char('.') + name);
        if (!pidFile.open(QIODevice::WriteOnly))
            return 2;
        const QByteArray pid = QByteArray::number(QCoreApplication::applicationPid());
        if (pidFile.write(pid) != pid.size() || !pidFile.commit())
            return 2;
        for (;;) {
            QThread::msleep(20);
        }
    }
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
    const int workers
        = option(QStringLiteral("--workers"), qEnvironmentVariable("SPOOL_TEST_WORKERS", "4")).toInt(&validWorkers);
    bool validTimeout = false;
    const int timeout = option(QStringLiteral("--timeout-ms"), QStringLiteral("180000")).toInt(&validTimeout);
    if (!validWorkers || workers < 1 || workers > 32 || !validTimeout || timeout < 1) {
        std::cerr << "workers must be 1..32 and timeout-ms must be positive\n";
        return 2;
    }
    const QString path
        = QFileInfo(option(QStringLiteral("--results"), QDir::current().filePath(QStringLiteral("test-results.json"))))
              .absoluteFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return 2;
    const bool resume
        = arguments.contains(QStringLiteral("--resume")) || qEnvironmentVariableIntValue("SPOOL_TEST_RESUME") != 0;
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
            QStringLiteral("fixture-access-violation"), QStringLiteral("fixture-forward-crash"),
            QStringLiteral("fixture-exit-three"), QStringLiteral("fixture-pass-b"), QStringLiteral("fixture-skip"),
            QStringLiteral("fixture-hang-tree") };
    } else {
        for (const auto& [name, entry] : SpoolTests::registry())
            names.append(QString::fromStdString(name));
    }
    const QString filter = option(QStringLiteral("--filter"), {});
    if (fixtures && filter.isEmpty())
        names.removeAll(QStringLiteral("fixture-hang-tree"));
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
            status = QStringLiteral("interrupted");
            row.insert(QStringLiteral("status"), status);
            const QString reason = QStringLiteral("supervisor interrupted while selector was running");
            row.insert(QStringLiteral("reason"), reason);
            QJsonArray attempts = row.value(QStringLiteral("attempts")).toArray();
            attempts.append(QJsonObject { { QStringLiteral("status"), status }, { QStringLiteral("reason"), reason },
                { QStringLiteral("startedMs"), row.value(QStringLiteral("startedMs")) },
                { QStringLiteral("log"), row.value(QStringLiteral("log")) } });
            row.insert(QStringLiteral("attempts"), attempts);
        }
        if (resume && status == QStringLiteral("crashed")) {
            row.insert(QStringLiteral("resumeSkipped"), true);
        } else if (!resume || status.isEmpty() || status == QStringLiteral("pending")
            || status == QStringLiteral("interrupted")
            || (retry
                && (status == QStringLiteral("failed") || status == QStringLiteral("timed-out")
                    || status == QStringLiteral("start-failed")))) {
            row.insert(QStringLiteral("status"), QStringLiteral("pending"));
            row.remove(QStringLiteral("reason"));
            row.remove(QStringLiteral("resumeSkipped"));
            pending.append(name);
        }
        results.insert(name, row);
    }
    state.insert(QStringLiteral("results"), results);
    if (!save(path, state))
        return 2;

    struct Running {
        QString name;
        std::unique_ptr<SelectorProcess> process;
        qint64 started;
        QElapsedTimer elapsed;
    };
    std::vector<Running> running;
    running.reserve(size_t(workers));
    int next = 0;
    int peakWorkers = 0;
    bool journalFailed = false;
    bool treeFailed = false;
#ifdef Q_OS_UNIX
    const auto previousTerm = std::signal(SIGTERM, interruptSupervisor);
    const auto previousInt = std::signal(SIGINT, interruptSupervisor);
#endif
    const auto persist = [&] {
        state.insert(QStringLiteral("results"), results);
        state.insert(QStringLiteral("peakWorkers"), peakWorkers);
        if (!save(path, state))
            journalFailed = true;
    };
    while (next < pending.size() || !running.empty()) {
#ifdef Q_OS_UNIX
        if (interruptedSignal) {
            // Leave running rows intact: resume records interrupted evidence,
            // not a genuine crash that this supervisor never observed.
            running.clear();
            std::signal(SIGTERM, previousTerm);
            std::signal(SIGINT, previousInt);
            return 1;
        }
#endif
        while (!journalFailed && next < pending.size() && int(running.size()) < workers) {
            const QString name = pending[next++];
            auto process = std::make_unique<SelectorProcess>();
            const int attempt = results.value(name).toObject().value(QStringLiteral("attempts")).toArray().size() + 1;
            const QString logPath
                = path + QLatin1Char('.') + name + QLatin1Char('.') + QString::number(attempt) + QStringLiteral(".log");
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
            row.remove(QStringLiteral("exitCode"));
            row.remove(QStringLiteral("finishedMs"));
            row.insert(QStringLiteral("startedMs"), started);
            row.insert(QStringLiteral("log"), logPath);
            results.insert(name, row);
            persist();
            if (journalFailed)
                break;
            QStringList childArguments { QStringLiteral("--child"), name };
            if (fixtures)
                childArguments.prepend(QStringLiteral("--fixture-suite"));
            if (process->ownershipReady())
                process->start(app.applicationFilePath(), childArguments);
            if (!process->ownershipReady() || !process->waitForStarted(10000)) {
                row.insert(QStringLiteral("status"), QStringLiteral("start-failed"));
                row.insert(QStringLiteral("reason"),
                    process->ownershipReady() ? process->errorString()
                                              : QStringLiteral("cannot establish isolated selector process tree"));
                row.insert(QStringLiteral("finishedMs"), QDateTime::currentMSecsSinceEpoch());
                QJsonArray attempts = row.value(QStringLiteral("attempts")).toArray();
                attempts.append(QJsonObject { { QStringLiteral("status"), QStringLiteral("start-failed") },
                    { QStringLiteral("startedMs"), started },
                    { QStringLiteral("finishedMs"), row.value(QStringLiteral("finishedMs")) },
                    { QStringLiteral("log"), logPath }, { QStringLiteral("exitCode"), -1 },
                    { QStringLiteral("reason"), row.value(QStringLiteral("reason")) } });
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
                treeFailed |= !it->process->stopTree();
                process.waitForFinished(10000);
            } else {
                process.waitForFinished(5);
            }
            if (process.state() != QProcess::NotRunning) {
                ++it;
                continue;
            }
            const bool cleanupFailed = !it->process->stopTree();
            treeFailed |= cleanupFailed;
            const QString status = timedOut                   ? QStringLiteral("timed-out")
                : process.exitStatus() == QProcess::CrashExit ? QStringLiteral("crashed")
                : !cleanupFailed && process.exitCode() == 0   ? QStringLiteral("passed")
                : cleanupFailed                               ? QStringLiteral("failed")
                : process.exitCode() == skipExitCode          ? QStringLiteral("skipped")
                                                              : QStringLiteral("failed");
            QJsonObject row = results.value(it->name).toObject();
            row.insert(QStringLiteral("status"), status);
            if (cleanupFailed)
                row.insert(QStringLiteral("reason"), QStringLiteral("cannot terminate selector process tree"));
            row.insert(QStringLiteral("exitCode"), process.exitCode());
            row.insert(QStringLiteral("finishedMs"), QDateTime::currentMSecsSinceEpoch());
            QJsonArray attempts = row.value(QStringLiteral("attempts")).toArray();
            attempts.append(
                QJsonObject { { QStringLiteral("status"), status }, { QStringLiteral("startedMs"), it->started },
                    { QStringLiteral("finishedMs"), row.value(QStringLiteral("finishedMs")) },
                    { QStringLiteral("exitCode"), process.exitCode() },
                    { QStringLiteral("log"), row.value(QStringLiteral("log")) } });
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
#ifdef Q_OS_UNIX
    std::signal(SIGTERM, previousTerm);
    std::signal(SIGINT, previousInt);
#endif
    bool failed = journalFailed || treeFailed;
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
    std::signal(SIGABRT, abortAsOsCrash);
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
        return fixture(QString::fromUtf8(argv[selectorIndex]), argc, argv);
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
        receipt.insert(QStringLiteral("status"),
            result == 0                  ? QStringLiteral("passed")
                : result == skipExitCode ? QStringLiteral("skipped")
                                         : QStringLiteral("failed"));
    }
    return result;
}
