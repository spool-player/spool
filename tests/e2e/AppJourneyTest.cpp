#include "JellyfinFixture.h"
#include "TestMain.h"

#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLibraryInfo>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRect>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
void turn(int milliseconds = 100)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
bool finish(QProcess& process, int milliseconds)
{
    if (process.state() == QProcess::NotRunning)
        return true;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), &loop, &QEventLoop::quit);
    QObject::connect(&process, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    deadline.start(milliseconds);
    loop.exec(); // The loopback server must keep servicing I/O while spoolet waits.
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(5000);
        return false;
    }
    return true;
}
QByteArray run(const QString& executable, const QStringList& arguments, const QProcessEnvironment& environment,
    const QByteArray& input = {}, int milliseconds = 15000)
{
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(executable, arguments);
    require(process.waitForStarted(5000), "required journey executable could not start");
    if (!input.isEmpty())
        process.write(input);
    process.closeWriteChannel();
    require(finish(process, milliseconds), "journey subprocess exceeded its deadline");
    require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
        "journey subprocess failed (output deliberately withheld)");
    return process.readAllStandardOutput();
}
QByteArray runOcr(const QString& executable, const QStringList& arguments, QProcessEnvironment environment)
{
    // A constrained simulator host can spend more time scheduling OpenMP
    // workers than reading the frame. Each OCR call owns a single worker.
    environment.insert("OMP_THREAD_LIMIT", "1");
    try {
        return run(executable, arguments, environment, {}, 30000);
    } catch (const std::runtime_error& error) {
        throw std::runtime_error(std::string("frame OCR: ") + error.what());
    }
}
QString executable(const char *variable, const QString& fallback)
{
    const QString requested = qEnvironmentVariable(variable);
    const QString result = requested.isEmpty() ? fallback : requested;
    require(!result.isEmpty() && QFileInfo(result).isExecutable(), "required built app or runtime tool is unavailable");
    return result;
}
QString sampleBlockedProcess(qint64 pid, const QString& directory, const QProcessEnvironment& environment)
{
#if defined(Q_OS_MACOS)
    const QString path = directory + "/blocked-process.sample";
    QFile file(path);
    if (pid <= 0 || !file.open(QIODevice::WriteOnly)
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return QStringLiteral("unavailable");
    file.close();
    try {
        run("/usr/bin/sample", { QString::number(pid), "1", "10", "-file", path }, environment, {}, 10000);
    } catch (const std::exception&) {
        QFile::remove(path);
        return QStringLiteral("unavailable");
    }
    QStringList symbols;
    int thread = 0;
    QStringList threadSymbols;
    const auto finishThread = [&] {
        for (const auto& symbol : threadSymbols)
            symbols.append(QStringLiteral("T%1:%2").arg(thread).arg(symbol));
        threadSymbols.clear();
    };
    if (file.open(QIODevice::ReadOnly)) {
        // A sample includes process paths and other metadata. Export only
        // known stack symbols, never its header, addresses or arbitrary text.
        const QStringList lines = QString::fromUtf8(file.readAll()).split('\n');
        for (const QString& line : lines) {
            if (line.contains("Thread_")) {
                finishThread();
                ++thread;
            }
            const QStringList words = line.simplified().split(' ');
            for (const QString& word : words) {
                const QString symbol = word.section('(', 0, 0);
                if (!symbol.startsWith("mpv_") && !symbol.startsWith("mp_") && !symbol.startsWith("vo_")
                    && !symbol.startsWith("Spool::") && !symbol.startsWith("QQuick") && !symbol.startsWith("QSG")
                    && !symbol.startsWith("QEvent") && !symbol.startsWith("QCoreApplication::")
                    && !symbol.startsWith("QIOS") && !symbol.startsWith("QRhi") && !symbol.startsWith("QOpenGL")
                    && !symbol.startsWith("CA::") && !symbol.startsWith("EAGL")
                    && !symbol.startsWith("presentRenderbuffer") && !symbol.startsWith("gl_")
                    && !symbol.startsWith("setproperty_fn") && !symbol.startsWith("set_pause_state")
                    && !symbol.startsWith("flip_page") && !symbol.startsWith("render_frame")
                    && !symbol.startsWith("draw_frame") && !symbol.startsWith("wait_until")
                    && !symbol.startsWith("lock_core") && !symbol.startsWith("run_locked")
                    && !symbol.startsWith("play_current_file") && !symbol.startsWith("write_video")
                    && !symbol.startsWith("grabWindow") && !symbol.startsWith("fill_audio_out_buffers")
                    && !symbol.startsWith("run_playloop") && !symbol.startsWith("AudioOutputUnit")
                    && !symbol.startsWith("AudioUnit") && !symbol.startsWith("AURemoteIO")
                    && !symbol.startsWith("AUIOClient") && !symbol.startsWith("ao_") && !symbol.startsWith("ca_")
                    && !symbol.startsWith("pl_") && !symbol.startsWith("__psynch_") && !symbol.startsWith("pthread_")
                    && !symbol.startsWith("_pthread_") && !symbol.startsWith("mach_msg"))
                    continue;
                if (symbol.size() > 160 || !std::all_of(symbol.cbegin(), symbol.cend(), [](QChar c) {
                        return c.isLetterOrNumber() || c == '_' || c == ':' || c == '~';
                    }))
                    continue;
                // sample prints root first. Retain the leaf-side waits, not
                // only the entry frames that explain nothing about the stall.
                threadSymbols.removeAll(symbol);
                threadSymbols.append(symbol);
                if (threadSymbols.size() > 12)
                    threadSymbols.removeFirst();
            }
        }
    }
    finishThread();
    file.close();
    QFile::remove(path);
    return symbols.isEmpty() ? QStringLiteral("unavailable") : symbols.join(',');
#else
    Q_UNUSED(pid);
    Q_UNUSED(directory);
    Q_UNUSED(environment);
    return QStringLiteral("unavailable");
#endif
}
class DeviceSession final {
public:
    DeviceSession(QString kind, const QString& directory, QProcessEnvironment environment)
        : kind(std::move(kind))
        , environment(std::move(environment))
        , hostDescriptor(directory + "/device.json")
    {
    }
    void start(const QString& origin, const QString& instance, const QString& ocr)
    {
        require(qEnvironmentVariable("SPOOL_E2E_ISOLATED_DEVICE") == "1",
            "mobile journey requires a driver-owned isolated emulator, never a user device");
        if (this->kind == "android") {
            device = qEnvironmentVariable("ANDROID_SERIAL");
            require(device.startsWith("emulator-"), "Android journey requires an explicit private emulator serial");
            tool = executable("SPOOL_E2E_ADB", QStandardPaths::findExecutable("adb"));
            bundle = "com.sachk.spool";
            const int reversePort = QUrl(origin).port();
            ownership("prepare", { { "reversePort", reversePort } });
            remoteScreenshot = "/data/user/0/" + bundle + "/cache/journey-frame.png";
        } else {
            require(this->kind == "tvos" || this->kind == "ios", "unknown mobile journey device");
            device = qEnvironmentVariable("SPOOL_TVOS_DEVICE");
            require(!device.isEmpty(), "Apple journey requires an explicit private simulator UUID");
            tool = executable("SPOOL_E2E_XCRUN", QStandardPaths::findExecutable("xcrun"));
            bundle = qEnvironmentVariable("SPOOL_E2E_TVOS_BUNDLE", "com.sachk.spool");
            container = QString::fromUtf8(runTool({ "get_app_container", device, bundle, "data" })).trimmed();
            require(QDir::isAbsolutePath(container) && QDir(container).exists(),
                "installed actual Apple app data container is unavailable");
            remoteScreenshot = container + "/Library/Caches/journey-frame.png";
            ownership("prepare");
        }
        assertDefaultAutomationOff(instance + "-default", ocr);
        launchProduct({ "--automation-port=0", "--instance=" + instance, "--provider-store=" + origin + '/' });
        QElapsedTimer deadline;
        deadline.start();
        QJsonObject descriptor;
        while (deadline.elapsed() < 60000) {
            QByteArray bytes;
            if (this->kind == "android") {
                try {
                    bytes = runTool(
                        { "shell", "-T", "run-as", bundle, "cat", "cache/spool-control/" + instance + ".json" });
                } catch (const std::runtime_error&) {
                    // The file is published only after the app initialized IPC.
                }
            } else {
                QFile file(container + "/Library/Caches/spool-control/" + instance + ".json");
                if (file.open(QIODevice::ReadOnly))
                    bytes = file.read(4097);
            }
            if (bytes.size() <= 4096)
                descriptor = QJsonDocument::fromJson(bytes).object();
            if (descriptor["transport"] == "tcp" && descriptor["endpoint"] == "127.0.0.1"
                && descriptor["token"].toString().size() == 64 && descriptor["instance"] == instance
                && descriptor["port"].toInt() > 0 && descriptor["port"].toInt() <= 65535)
                break;
            turn(200);
        }
        require(descriptor["transport"] == "tcp" && descriptor["endpoint"] == "127.0.0.1"
                && descriptor["token"].toString().size() == 64 && descriptor["instance"] == instance
                && descriptor["port"].toInt() > 0 && descriptor["port"].toInt() <= 65535,
            "actual mobile app did not publish its private authenticated automation descriptor");
        require(descriptor["pid"].toInteger() == processPid,
            "actual mobile app capability did not match the launched OS process identity");
        if (this->kind == "android") {
            const int forwardPort = ownership("forward", { { "port", descriptor["port"].toInt() } })["port"].toInt();
            require(forwardPort > 0 && forwardPort <= 65535, "private Android control port could not be forwarded");
            descriptor.insert("port", forwardPort);
        }
        QSaveFile file(hostDescriptor);
        const QByteArray bytes = QJsonDocument(descriptor).toJson(QJsonDocument::Compact);
        require(file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                && file.write(bytes) == bytes.size() && file.commit(),
            "pulled descriptor could not be stored privately");
        ownership("ready", { { "descriptor", hostDescriptor } });
    }
    ~DeviceSession()
    {
        try {
            ownership("cleanup", { { "artifacts", QFileInfo(hostDescriptor).absolutePath() } });
        } catch (const std::exception&) {
            // The adapter remains the authoritative owner after any host exit.
        }
    }
    void propagateObservedCrash()
    {
        if (processPid <= 0 || productRunning())
            return;
        // CrashReporter can publish slightly after the control socket closes.
        // Only an OS signal receipt can classify this as a crash; absent IPC
        // or an absent process alone remains an ordinary journey failure.
        QElapsedTimer deadline;
        deadline.start();
        do {
            const int signal = crashSignal();
            if (signal > 0) {
                QJsonObject evidence { { "platform", kind }, { "pid", processPid }, { "signal", signal } };
                if (kind == "android")
                    addAndroidCrashDetails(evidence);
                QSaveFile file(QFileInfo(hostDescriptor).dir().filePath("mobile-crash-evidence.json"));
                const QByteArray bytes = QJsonDocument(evidence).toJson(QJsonDocument::Compact);
                const bool saved = file.open(QIODevice::WriteOnly)
                    && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                    && file.write(bytes) == bytes.size() && file.commit();
                std::cerr << "app-journey: actual mobile product OS crash, signal=" << signal
                          << "; private artifacts retained at "
                          << QFileInfo(hostDescriptor).absolutePath().toStdString()
                          << (saved ? "" : "; crash metadata could not be saved") << std::endl;
                SpoolTests::propagateCrash(signal);
            }
            turn(200);
        } while (deadline.elapsed() < 5000);
    }
    QString descriptorPath() const
    {
        return hostDescriptor;
    }
    QString blockedProcessSample() const
    {
        return sampleBlockedProcess(processPid, QFileInfo(hostDescriptor).absolutePath(), environment);
    }
    QString screenshotPath() const
    {
        return remoteScreenshot;
    }
    void androidTvRemoteKey(const QString& key)
    {
        require(kind == "android" && qEnvironmentVariable("SPOOL_E2E_ANDROID_TV") == "1",
            "Android TV remote input requires the isolated TV emulator");
        runTool({ "shell", "input", "keyevent", key });
    }
    void deleteScreenshot()
    {
        if (kind == "android") {
            runTool({ "shell", "-T", "run-as", bundle, "rm", "-f", "cache/journey-frame.png" });
        } else {
            require(!QFileInfo::exists(remoteScreenshot) || QFile::remove(remoteScreenshot),
                "previous actual simulator screenshot could not be removed");
        }
    }
    void pullScreenshot(const QString& destination)
    {
        QByteArray bytes;
        if (kind == "android")
            bytes = runTool({ "shell", "-T", "run-as", bundle, "cat", "cache/journey-frame.png" });
        else {
            QFile input(remoteScreenshot);
            require(input.open(QIODevice::ReadOnly), "actual simulator screenshot is unavailable");
            bytes = input.readAll();
        }
        QSaveFile output(destination);
        require(output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit(),
            "actual mobile GPU screenshot could not be pulled");
    }

private:
    static qsizetype firstJsonEnd(const QByteArray& bytes)
    {
        int depth = 0;
        bool quoted = false;
        bool escaped = false;
        for (qsizetype index = 0; index < bytes.size(); ++index) {
            const char character = bytes[index];
            if (quoted) {
                if (escaped)
                    escaped = false;
                else if (character == '\\')
                    escaped = true;
                else if (character == '"')
                    quoted = false;
            } else if (character == '"') {
                quoted = true;
            } else if (character == '{') {
                ++depth;
            } else if (character == '}' && --depth == 0) {
                return index + 1;
            }
        }
        return 0;
    }
    void addAndroidCrashDetails(QJsonObject& evidence)
    {
        QJsonArray frames;
        try {
            // Android emits the backtrace from crash_dump, whose logging PID
            // differs from the product PID recorded in the tombstone header.
            const QString logs = QString::fromUtf8(
                runTool({ "logcat", "-d", "-b", "crash", "-t", "600", "-v", "epoch", "DEBUG:I", "*:S" }));
            const QRegularExpression lineFormat(
                QStringLiteral("^\\s*([0-9]+(?:\\.[0-9]+)?)\\s+([0-9]+)\\s+[0-9]+\\s+[A-Z]\\s+DEBUG\\s*:\\s*(.*)$"));
            const QRegularExpression product(
                QStringLiteral("^pid: %1, .* >>> %2 <<<$").arg(processPid).arg(QRegularExpression::escape(bundle)));
            const QRegularExpression frame(QStringLiteral(
                "^#[0-9]+\\s+pc\\s+([0-9a-f]{1,16})\\s+(?:[^\\s]*/)?(lib(?:spool_x86_64|"
                "Qt6(?:Core|Gui|Quick|Qml|OpenGL)|mpv|c|EGL|GLESv2|art|android_runtime)\\.so)(?:\\s|$)"));
            qint64 reporter = 0;
            for (const QString& line : logs.split('\n')) {
                const auto record = lineFormat.match(line.trimmed());
                if (!record.hasMatch() || record.captured(1).toDouble() < deviceLaunchEpoch)
                    continue;
                const auto message = record.captured(3);
                if (message.startsWith(QLatin1String("pid: "))) {
                    reporter = product.match(message).hasMatch() ? record.captured(2).toLongLong() : 0;
                    continue;
                }
                if (reporter == 0 || record.captured(2).toLongLong() != reporter)
                    continue;
                if (message.startsWith(
                        QLatin1String("Abort message: 'FORTIFY: pthread_mutex_lock called on a destroyed mutex")))
                    evidence.insert("abortKind", "destroyed-mutex");
                const auto match = frame.match(message);
                if (match.hasMatch())
                    frames.append(QJsonObject { { "module", match.captured(2) }, { "offset", match.captured(1) } });
                if (frames.size() == 12)
                    break;
            }
        } catch (const std::runtime_error&) {
            // The already confirmed signal remains usable without a stack.
        }
        evidence.insert("frames", frames);
    }
    int crashSignal()
    {
        try {
            if (kind == "android") {
                const QString logs = QString::fromUtf8(runTool({ "logcat", "-d", "-b", "crash", "-b", "main", "--pid",
                    QString::number(processPid), "-v", "epoch" }));
                const QRegularExpression stamp(QStringLiteral("^\\s*([0-9]+(?:\\.[0-9]+)?)\\s+([0-9]+)\\s+"));
                const QRegularExpression fatal(QStringLiteral("\\bFatal signal ([1-9][0-9]?)\\b"));
                for (const QString& line : logs.split('\n')) {
                    const auto time = stamp.match(line);
                    const auto signal = fatal.match(line);
                    if (time.hasMatch() && signal.hasMatch() && time.captured(1).toDouble() >= deviceLaunchEpoch
                        && time.captured(2).toLongLong() == processPid) {
                        const int value = signal.captured(1).toInt();
                        if (value <= 64)
                            return value;
                    }
                }
                return 0;
            }
#if defined(Q_OS_MACOS)
            const QDir reports(QDir::home().filePath("Library/Logs/DiagnosticReports"));
            for (const QFileInfo& info : reports.entryInfoList({ "Spool*.ips" }, QDir::Files, QDir::Time)) {
                if (info.lastModified().toUTC() < launchTime || info.size() > 1024 * 1024)
                    continue;
                QFile file(info.filePath());
                if (!file.open(QIODevice::ReadOnly))
                    continue;
                const QByteArray bytes = file.readAll();
                const qsizetype boundary = firstJsonEnd(bytes);
                if (boundary <= 0 || !QJsonDocument::fromJson(bytes.left(boundary)).isObject())
                    continue;
                // Apple's .ips format is metadata JSON followed by payload JSON,
                // not a single JSON document.
                const auto payload = QJsonDocument::fromJson(bytes.mid(boundary)).object();
                if (payload["pid"].toInteger() != processPid
                    || !payload["procPath"].toString().contains(device, Qt::CaseInsensitive))
                    continue;
                const auto termination = payload["termination"].toObject();
                if (termination["namespace"] == "SIGNAL") {
                    const int value = termination["code"].toInt();
                    if (value > 0 && value < NSIG)
                        return value;
                }
                // AMFI/code-signing deaths use namespace CODESIGNING and code
                // is NOT a signal number. Preserve the actual exception signal.
                const QString name = payload["exception"].toObject()["signal"].toString().section(' ', 0, 0);
                static constexpr std::pair<const char *, int> signalMappings[] { { "SIGABRT", SIGABRT },
                    { "SIGBUS", SIGBUS }, { "SIGFPE", SIGFPE }, { "SIGILL", SIGILL }, { "SIGKILL", SIGKILL },
                    { "SIGSEGV", SIGSEGV }, { "SIGSYS", SIGSYS }, { "SIGTRAP", SIGTRAP } };
                for (const auto& signal : signalMappings)
                    if (name == QLatin1String(signal.first))
                        return signal.second;
            }
#endif
        } catch (const std::runtime_error&) {
            // An unavailable OS diagnostic is not invented crash evidence.
        }
        return 0;
    }
    void launchProduct(const QStringList& arguments)
    {
        launchTime = QDateTime::currentDateTimeUtc();
        const auto launched = ownership("launch", { { "arguments", QJsonArray::fromStringList(arguments) } });
        processPid = launched["pid"].toInteger();
        deviceLaunchEpoch = launched["epoch"].toInteger();
        require(processPid > 0, "actual mobile product launch did not expose its OS process identity");
    }
    void stopProduct()
    {
        ownership("stop");
    }
    bool productRunning()
    {
        try {
            if (kind == "android")
                return QString::fromUtf8(runTool({ "shell", "pidof", "-s", bundle })).trimmed().toLongLong()
                    == processPid;
#if defined(Q_OS_MACOS)
            // Simulator applications are host processes. Signal zero observes
            // the launch PID without sending a signal or requiring root.
            return ::kill(static_cast<pid_t>(processPid), 0) == 0 || errno == EPERM;
#else
            throw std::runtime_error("tvOS process observation requires a local macOS simulator");
#endif
        } catch (const std::runtime_error&) {
            return false;
        }
    }
    void assertDefaultAutomationOff(const QString& instance, const QString& ocr)
    {
        launchProduct({ "--instance=" + instance });
        const QString screenshot = QFileInfo(hostDescriptor).dir().filePath("mobile-default-startup.png");
        QElapsedTimer deadline;
        deadline.start();
        bool rendered = false;
        while (deadline.elapsed() < 60000) {
            if (!productRunning()) {
                propagateObservedCrash();
                throw std::runtime_error("actual mobile product exited during its normal UI startup");
            }
            require(!QFileInfo::exists(screenshot) || QFile::remove(screenshot),
                "previous normal-startup screenshot could not be removed");
            if (kind == "android") {
                const QByteArray bytes = runTool({ "exec-out", "screencap", "-p" });
                QSaveFile file(screenshot);
                require(file.open(QIODevice::WriteOnly)
                        && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                        && file.write(bytes) == bytes.size() && file.commit(),
                    "actual normal Android UI screenshot could not be retained privately");
            } else {
                runTool({ "io", device, "screenshot", "--type=png", screenshot });
                require(QFile::setPermissions(screenshot, QFileDevice::ReadOwner | QFileDevice::WriteOwner),
                    "actual normal simulator UI screenshot could not be made private");
            }
            const QImage image(screenshot);
            require(!image.isNull(), "actual mobile normal-startup screenshot was not an image");
            for (const QString& segmentation : { QStringLiteral("11"), QStringLiteral("6") }) {
                const QByteArray text
                    = runOcr(ocr, { screenshot, "stdout", "-l", "eng", "--psm", segmentation }, environment);
                if (QString::fromUtf8(text).simplified().contains("Welcome to Spool", Qt::CaseInsensitive)) {
                    rendered = true;
                    break;
                }
            }
            if (rendered)
                break;
            turn(200);
        }
        require(rendered && productRunning(), "actual mobile normal launch did not render its product UI");
        if (kind == "android") {
            const QByteArray listing = runTool({ "shell", "-T", "run-as", bundle, "sh", "-c",
                "'if [ -d cache/spool-control ]; then find cache/spool-control -maxdepth 1 -name \"*.json\" -type f; "
                "fi'" });
            const auto files = QString::fromUtf8(listing).split('\n', Qt::SkipEmptyParts);
            require(files.size() <= 64, "normal app startup published excessive control discovery entries");
            for (const QString& name : files) {
                require(QFileInfo(name).fileName() != instance + ".json",
                    "actual mobile normal startup published its unrequested control descriptor");
                require(name.startsWith("cache/spool-control/") && name.endsWith(".json"),
                    "normal-startup discovery file escaped the private app cache");
                const auto entry
                    = QJsonDocument::fromJson(runTool({ "shell", "-T", "run-as", bundle, "cat", name })).object();
                require(entry["instance"] != instance && entry["pid"].toInteger() != processPid,
                    "actual mobile normal startup advertised an unrequested automation listener");
            }
        } else {
            const QDir registry(container + "/Library/Caches/spool-control");
            const QStringList files = registry.entryList({ "*.json" }, QDir::Files);
            require(files.size() <= 64, "normal app startup published excessive control discovery entries");
            for (const QString& name : files) {
                require(name != instance + ".json",
                    "actual mobile normal startup published its unrequested control descriptor");
                QFile file(registry.filePath(name));
                require(file.open(QIODevice::ReadOnly), "private normal-startup discovery entry could not be read");
                const auto entry = QJsonDocument::fromJson(file.read(4097)).object();
                require(entry["instance"] != instance && entry["pid"].toInteger() != processPid,
                    "actual mobile normal startup advertised an unrequested automation listener");
            }
        }
        stopProduct();
    }
    QJsonObject ownership(const QString& operation, const QJsonObject& value = {})
    {
        QFile capability(environment.value("SPOOL_E2E_DEVICE_JOURNEY_OWNER"));
        require(capability.open(QIODevice::ReadOnly) && capability.size() <= 4096,
            "mobile journey requires its private adapter ownership capability");
        const auto descriptor = QJsonDocument::fromJson(capability.readAll()).object();
        require(descriptor["format"].toInt() == 1 && descriptor["platform"] == kind
                && descriptor["token"].toString().size() == 64 && descriptor["nonce"].toString().size() == 32
                && descriptor["port"].toInt() > 0 && descriptor["port"].toInt() <= 65535,
            "private adapter ownership capability is invalid");
        QTcpSocket socket;
        socket.connectToHost(QHostAddress::LocalHost, descriptor["port"].toInt());
        require(socket.waitForConnected(5000), "mobile journey adapter ownership service is unavailable");
        const QJsonObject request { { "nonce", descriptor["nonce"] }, { "token", descriptor["token"] },
            { "operation", operation }, { "value", value } };
        const QByteArray bytes = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
        require(socket.write(bytes) == bytes.size(), "adapter ownership request could not be written");
        QElapsedTimer deadline;
        deadline.start();
        QByteArray response;
        while (!response.contains('\n') && deadline.elapsed() < 90000) {
            // Keep the production-provider fixture's event loop alive while
            // OS launch and real-controller readiness requests are in flight.
            turn(25);
            response += socket.readAll();
            require(response.size() <= 65536, "adapter ownership response exceeded its bound");
            if (socket.state() == QAbstractSocket::UnconnectedState)
                break;
        }
        const auto envelope = QJsonDocument::fromJson(response).object();
        require(envelope["ok"].toBool(), "adapter rejected a mobile journey ownership operation");
        return envelope["result"].toObject();
    }
    QByteArray runTool(QStringList arguments)
    {
        if (kind == "android")
            arguments = QStringList { "-s", device } + arguments;
        else
            arguments.prepend("simctl");
        return run(tool, arguments, environment, {}, 30000);
    }
    QString kind;
    QProcessEnvironment environment;
    QString hostDescriptor;
    QString device;
    QString tool;
    QString bundle;
    QString container;
    QString remoteScreenshot;
    qint64 processPid = 0;
    QDateTime launchTime;
    qint64 deviceLaunchEpoch = 0;
};
enum class TextSurface {
    Window,
    LibraryRow,
    RelatedCard,
    DetailPrimaryCaption,
    PlayerOptions,
    PlayerRepeatDetail,
    PlayerSleepDetail
};
enum class TextPreprocess { Original, Neutral, Inverse, SmoothInverse, NeutralAdaptive, Positive, Adaptive };
struct Words {
    QString text;
    QList<qsizetype> starts;
    QList<QRect> rectangles;

    QRect phraseBounds(const QString& phrase) const
    {
        const qsizetype first = text.indexOf(phrase, 0, Qt::CaseInsensitive);
        if (first < 0)
            return {};
        const qsizetype last = first + phrase.size();
        QRect result;
        for (qsizetype index = 0; index < starts.size(); ++index) {
            const qsizetype end = index + 1 < starts.size() ? starts[index + 1] - 1 : text.size();
            if (starts[index] < last && end > first)
                result = result.united(rectangles[index]);
        }
        return result;
    }
};
class Journey final {
public:
    Journey(QString control, QProcessEnvironment environment, const QString& directory)
        : control(std::move(control))
        , environment(std::move(environment))
        , screenshotPath(directory + "/frame.png")
        , ocr(executable("SPOOL_E2E_TESSERACT", QStandardPaths::findExecutable("tesseract")))
    {
    }
    ~Journey()
    {
        if (app.state() != QProcess::NotRunning) {
            app.terminate();
            if (!finish(app, 10000)) {
                app.kill();
                app.waitForFinished(5000);
            }
        }
    }
    void launch(const QString& executable, const QString& origin, const QString& deviceKind)
    {
        if (deviceKind.isEmpty()) {
            app.setProcessEnvironment(environment);
            const auto privateLog = [&](const char *name) {
                const QString path = QFileInfo(screenshotPath).dir().filePath(QString::fromLatin1(name));
                QFile file(path);
                require(file.open(QIODevice::WriteOnly)
                        && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner),
                    "private app process log could not be created");
                return path;
            };
            app.setStandardOutputFile(privateLog("app.stdout.log"));
            app.setStandardErrorFile(privateLog("app.stderr.log"));
            app.start(executable,
                { "--instance", instance, "--data-dir", environment.value("SPOOL_DATA_HOME"), "--provider-store",
                    origin + '/' });
            require(app.waitForStarted(10000), "built Spool could not start");
        } else {
            device = std::make_unique<DeviceSession>(deviceKind, QFileInfo(screenshotPath).absolutePath(), environment);
            device->start(origin, instance, ocr);
        }
        await(
            [&] {
                const auto reply = command({ "status" }, {}, false);
                return reply["initialized"].toBool() && reply["window"].toObject()["exposed"].toBool();
            },
            "Spool did not initialize an exposed GPU window", 60000);
    }
    QJsonObject command(const QStringList& args, const QByteArray& input = {}, bool required = true,
        const QString& descriptorOverride = {}, QJsonObject *envelope = nullptr, bool observeAfterTimeout = false)
    {
        ensureRunning();
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setStandardErrorFile(QProcess::nullDevice());
        const QStringList destination = !descriptorOverride.isEmpty()
            ? QStringList { "--descriptor", descriptorOverride }
            : device ? QStringList { "--descriptor", device->descriptorPath() }
                     : QStringList { "--instance", instance };
        QElapsedTimer requestClock;
        requestClock.start();
        // Match LocalControl::RequestTimeoutMs (10s) across both desktop and
        // mobile targets: CI runners using software OpenGL renderers (such as
        // Apple Software Renderer on macOS or emulated OpenGL ES on mobile)
        // share the main thread with playback setup/frames and can transiently
        // push a single control round-trip slightly past 5s.
        constexpr int commandTimeoutMs = 10000;
        process.start(control, destination + QStringList { "--timeout", QString::number(commandTimeoutMs) } + args);
        require(process.waitForStarted(5000), "built spoolet could not start");
        if (!input.isEmpty())
            process.write(input);
        process.closeWriteChannel();
        require(finish(process, commandTimeoutMs + 5000), "spoolet exceeded its deadline");
        const qint64 requestElapsedMs = requestClock.elapsed();
        ensureRunning();
        const auto response = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        const QString transportCode = response["error"].toObject()["code"].toString();
        if (device
            && (transportCode == "unavailable" || transportCode == "timeout" || transportCode == "invalid_response"))
            device->propagateObservedCrash();
        if (envelope)
            *envelope = response;
        if (observeAfterTimeout && transportCode == "timeout"
            && response["error"].toObject()["message"] == "Instance did not complete command in time")
            return {};
        if (required
            && (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || !response["ok"].toBool())) {
            const QString code = response["error"].toObject()["code"].toString();
            const bool safeCode = code.size() <= 64 && std::all_of(code.cbegin(), code.cend(), [](QChar character) {
                return character == '_' || (character >= 'a' && character <= 'z');
            });
            // Client and server deadlines share the "timeout" code. Classify
            // only known protocol messages; never print an arbitrary payload.
            QString timeoutOrigin = QStringLiteral("unknown");
            if (code == "timeout") {
                const QString message = response["error"].toObject()["message"].toString();
                if (message == "Request did not complete before the server deadline")
                    timeoutOrigin = QStringLiteral("server");
                else if (message == "Command write timed out")
                    timeoutOrigin = QStringLiteral("client_write");
                else if (message == "Instance did not complete command in time")
                    timeoutOrigin = QStringLiteral("client_response");
            }
            QString geometry;
            if (args.value(0) == "screenshot" && code == "not_exposed") {
                const auto window = command({ "status" }, {}, false)["window"].toObject();
                geometry = QString(" exposed=%1 width=%2 height=%3")
                               .arg(window["exposed"].toBool())
                               .arg(window["width"].toInt())
                               .arg(window["height"].toInt());
            }
            throw std::runtime_error(
                QString("production spoolet rejected %1 (exit=%2, code=%3, elapsed_ms=%4, timeout_origin=%5)%6")
                    .arg(args.value(0))
                    .arg(process.exitCode())
                    .arg(safeCode ? code : QStringLiteral("invalid_response"))
                    .arg(requestElapsedMs)
                    .arg(timeoutOrigin)
                    .arg(geometry)
                    .toStdString());
        }
        return response["ok"].toBool() ? response["result"].toObject() : QJsonObject {};
    }
    void assertControlBoundaries()
    {
        const QString genuine = device
            ? device->descriptorPath()
            : environment.value("SPOOL_DATA_HOME") + "/runtime/spool-control/" + instance + ".json";
        QFile input(genuine);
        require(input.open(QIODevice::ReadOnly), "actual app descriptor could not be read for the security control");
        QJsonObject forged = QJsonDocument::fromJson(input.read(4097)).object();
        require(forged["token"].toString().size() == 64, "actual app descriptor has no capability");
        forged.insert("token", QString(64, QLatin1Char('0')));
        const QString deniedPath = QFileInfo(genuine).dir().filePath("denied-capability.json");
        QSaveFile output(deniedPath);
        const QByteArray bytes = QJsonDocument(forged).toJson(QJsonDocument::Compact);
        require(output.open(QIODevice::WriteOnly)
                && output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                && output.write(bytes) == bytes.size() && output.commit(),
            "negative capability could not be stored privately");
        QJsonObject envelope;
        command({ "text", "--stdin" }, "unauthorized-input", false, deniedPath, &envelope);
        require(!envelope["ok"].toBool() && envelope["error"].toObject()["code"] == "unauthorized",
            "actual app did not reject the wrong capability before text dispatch");
        command({ "text", "--stdin" }, "noneditable-input", false, {}, &envelope);
        require(!envelope["ok"].toBool() && envelope["error"].toObject()["code"] == "no_text_focus",
            "actual app accepted IME input without an editable focus");
        require(state()["initialized"].toBool(), "genuine control capability stopped working after denial");
    }
    void text(const QString& value, bool activateTvField = false)
    {
        const auto input = value.toUtf8();
        await(
            [&] {
                QJsonObject response;
                try {
                    require(command({ "text", "--stdin" }, input, true, {}, &response)["accepted"].toBool(),
                        "focused editable field did not accept production IME input");
                } catch (const std::exception&) {
                    if (!activateTvField || response["error"].toObject()["code"] != "no_text_focus")
                        throw;
                    // The TV focus ring can belong to the row while its editor is inactive.
                    // The rejection precedes dispatch, so pressing Select and retrying cannot duplicate text.
                    command({ "key", "ok" });
                    return false;
                }
                return true;
            },
            "could not enter text in the intended form field", 5000);
    }
    QJsonObject state(bool observeAfterTimeout = false)
    {
        return command({ "state" }, {}, true, {}, nullptr, observeAfterTimeout);
    }
    QImage capture(bool video = false)
    {
        QJsonObject result;
        await(
            [&] {
                const auto window = state(true)["window"].toObject();
                if (!window["exposed"].toBool() || window["width"].toInt() <= 0 || window["height"].toInt() <= 0)
                    return false;
                for (const QString& previous : { screenshotPath, libraryTextPath() })
                    require(!QFileInfo::exists(previous) || QFile::remove(previous),
                        "previous host screenshot could not be removed");
                if (device)
                    device->deleteScreenshot();
                QJsonObject response;
                try {
                    result = command(
                        { "screenshot", device ? device->screenshotPath() : screenshotPath }, {}, true, {}, &response);
                } catch (const std::exception&) {
                    // Exposure can change between separate IPC requests while
                    // the real platform replaces a startup/fullscreen surface.
                    // All other failures keep the command's safe deadline/code
                    // diagnostics rather than losing the failing capture stage.
                    if (response["error"].toObject()["code"] != "not_exposed")
                        throw;
                    return false;
                }
                return true;
            },
            "actual product window did not expose and capture a rendered framebuffer");
        require(result["frameAwaited"].toBool(), "capture did not await a rendered frame");
        if (video)
            require(result["videoIncluded"].toBool(), "capture excludes native video; cannot claim GPU frame proof");
        capturedState = result["state"].toObject();
        if (device)
            device->pullScreenshot(screenshotPath);
        QImage image(screenshotPath);
        require(!image.isNull(), "rendered screenshot is missing");
        if (device) {
            const auto window = capturedState["window"].toObject();
            const double ratio = window["devicePixelRatio"].toDouble();
            require(ratio > 0.0 && window["width"].toInt() > 0 && window["height"].toInt() > 0
                    && image.width() == qRound(window["width"].toInt() * ratio)
                    && image.height() == qRound(window["height"].toInt() * ratio)
                    && image.width() == result["width"].toInt() && image.height() == result["height"].toInt(),
                "mobile capture must match the actual exposed window's physical framebuffer dimensions");
        } else {
            require(image.width() >= 640 && image.height() >= 360, "desktop rendered screenshot is too small");
        }
        return image;
    }
    QList<Words> words(const QString& imagePath, int mode, const QPoint& origin = {},
        TextPreprocess preprocess = TextPreprocess::Original)
    {
        QString inputPath = imagePath;
        const bool neutralText = preprocess == TextPreprocess::Neutral;
        const bool highContrast = preprocess == TextPreprocess::Inverse;
        const bool smoothInverse = preprocess == TextPreprocess::SmoothInverse;
        const bool neutralAdaptive = preprocess == TextPreprocess::NeutralAdaptive;
        const bool positiveContrast = preprocess == TextPreprocess::Positive;
        const bool adaptiveContrast = preprocess == TextPreprocess::Adaptive;
        const bool needsPreprocess
            = neutralText || highContrast || smoothInverse || neutralAdaptive || positiveContrast || adaptiveContrast;
        const int linePadding = mode == 7 && smoothInverse ? 16 : 0;
        int scale = mode == 7                                                                       ? 4
            : mode == 6 || smoothInverse || neutralAdaptive || positiveContrast || adaptiveContrast ? 2
                                                                                                    : 1;
        QImage image(imagePath);
        require(!image.isNull(), "captured OCR image could not be read");
        qreal sourceScale = 1.0;
        if (device && std::max(image.width(), image.height()) >= 2000) {
            // DPR-3 simulator captures contain far more pixels than OCR needs.
            // Keep the full-resolution visual evidence and map recognized
            // bounds back to it after a single 2x downsample.
            image
                = image.scaled(image.width() / 2, image.height() / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            sourceScale = 0.5;
            scale = 1;
            inputPath = imagePath + QStringLiteral(".ocr-base.png");
            QSaveFile base(inputPath);
            require(base.open(QIODevice::WriteOnly)
                    && base.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) && image.save(&base, "PNG")
                    && base.commit(),
                "captured OCR image could not be downsampled");
        } else if (std::max(image.width(), image.height()) >= 2000) {
            scale = 1;
        }
        if (scale > 1 || needsPreprocess) {
            // Small captions beside a focus outline can be omitted even by
            // text-block segmentation. Resample only the actual framebuffer;
            // recognized bounds below return to its physical pixel space.
            if (neutralText) {
                // Chromatic artwork and focus outlines can merge with neutral
                // glyphs. Remove only that noise; never synthesize label pixels.
                image = image.convertToFormat(QImage::Format_RGB32);
                for (int y = 0; y < image.height(); ++y) {
                    auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(y));
                    for (int x = 0; x < image.width(); ++x) {
                        const QRgb pixel = pixels[x];
                        if (std::max({ qRed(pixel), qGreen(pixel), qBlue(pixel) })
                                - std::min({ qRed(pixel), qGreen(pixel), qBlue(pixel) })
                            > 32)
                            pixels[x] = qRgb(0, 0, 0);
                    }
                }
            }
            if (positiveContrast || adaptiveContrast) {
                // Preserve standard BT.601 luma for adaptive thresholding, or
                // isolate dark captions inside bright/coloured controls onto a
                // clean white canvas so dark-theme backgrounds cannot invert OCR.
                const QImage source = image.convertToFormat(QImage::Format_RGB32);
                const int width = source.width();
                const int height = source.height();
                image = QImage(width, height, QImage::Format_Grayscale8);
                if (adaptiveContrast) {
                    for (int y = 0; y < height; ++y) {
                        const auto *sourcePixels = reinterpret_cast<const QRgb *>(source.constScanLine(y));
                        auto *pixels = image.scanLine(y);
                        for (int x = 0; x < width; ++x) {
                            const QRgb pixel = sourcePixels[x];
                            pixels[x] = (77 * qRed(pixel) + 150 * qGreen(pixel) + 29 * qBlue(pixel) + 128) >> 8;
                        }
                    }
                } else {
                    std::vector<uint8_t> lumas(size_t(width) * size_t(height));
                    std::vector<uint8_t> insideH(size_t(width) * size_t(height), 0);
                    constexpr int kBrightLuma = 96;
                    constexpr int kMaxStrokeH = 24;
                    constexpr int kMaxStrokeV = 18;
                    for (int y = 0; y < height; ++y) {
                        const auto *sourcePixels = reinterpret_cast<const QRgb *>(source.constScanLine(y));
                        const size_t row = size_t(y) * size_t(width);
                        int lastBright = -kMaxStrokeH - 1;
                        for (int x = 0; x < width; ++x) {
                            const QRgb pixel = sourcePixels[x];
                            const uint8_t luma
                                = uint8_t((77 * qRed(pixel) + 150 * qGreen(pixel) + 29 * qBlue(pixel) + 128) >> 8);
                            lumas[row + size_t(x)] = luma;
                            if (luma >= kBrightLuma)
                                lastBright = x;
                            if (x - lastBright <= kMaxStrokeH)
                                insideH[row + size_t(x)] = 1;
                        }
                        int nextBright = width + kMaxStrokeH + 1;
                        for (int x = width - 1; x >= 0; --x) {
                            if (lumas[row + size_t(x)] >= kBrightLuma)
                                nextBright = x;
                            if (nextBright - x > kMaxStrokeH)
                                insideH[row + size_t(x)] = 0;
                        }
                    }
                    std::vector<uint8_t> insideV(size_t(width) * size_t(height), 0);
                    for (int x = 0; x < width; ++x) {
                        int lastBright = -kMaxStrokeV - 1;
                        for (int y = 0; y < height; ++y) {
                            const size_t idx = size_t(y) * size_t(width) + size_t(x);
                            if (lumas[idx] >= kBrightLuma)
                                lastBright = y;
                            if (y - lastBright <= kMaxStrokeV)
                                insideV[idx] = 1;
                        }
                        int nextBright = height + kMaxStrokeV + 1;
                        for (int y = height - 1; y >= 0; --y) {
                            const size_t idx = size_t(y) * size_t(width) + size_t(x);
                            if (lumas[idx] >= kBrightLuma)
                                nextBright = y;
                            if (nextBright - y > kMaxStrokeV)
                                insideV[idx] = 0;
                        }
                    }
                    for (int y = 0; y < height; ++y) {
                        const size_t row = size_t(y) * size_t(width);
                        auto *pixels = image.scanLine(y);
                        for (int x = 0; x < width; ++x) {
                            const size_t idx = row + size_t(x);
                            if (insideH[idx] || insideV[idx]) {
                                const int luma = lumas[idx];
                                // Keep anti-aliased glyph edges on mid-bright accent buttons
                                // (luma ~120) intact instead of clipping them to pure white.
                                const int scaled = (luma - 20) * 255 / (180 - 20);
                                pixels[x] = uint8_t(std::clamp(scaled, 0, 255));
                            } else {
                                pixels[x] = 255;
                            }
                        }
                    }
                }
            } else if (smoothInverse) {
                // Preserve anti-aliased light glyphs on dark controls (e.g.
                // flat buttons on compact mobile viewports) by smoothly mapping
                // dark theme surfaces to white ink background.
                const QImage source = image.convertToFormat(QImage::Format_RGB32);
                const int w = source.width();
                const int h = source.height();
                image = QImage(w, h, QImage::Format_Grayscale8);
                int minimumLuma = 255;
                int maximumLuma = 0;
                for (int y = 0; y < h; ++y) {
                    const auto *sourcePixels = reinterpret_cast<const QRgb *>(source.constScanLine(y));
                    auto *pixels = image.scanLine(y);
                    for (int x = 0; x < w; ++x) {
                        const QRgb pixel = sourcePixels[x];
                        const int luma = (77 * qRed(pixel) + 150 * qGreen(pixel) + 29 * qBlue(pixel) + 128) >> 8;
                        if (linePadding) {
                            minimumLuma = std::min(minimumLuma, luma);
                            maximumLuma = std::max(maximumLuma, luma);
                            pixels[x] = uint8_t(luma);
                        } else {
                            const int scaled = (180 - luma) * 255 / (180 - 40);
                            pixels[x] = uint8_t(std::clamp(scaled, 0, 255));
                        }
                    }
                }
                if (linePadding) {
                    const int range = std::max(1, maximumLuma - minimumLuma);
                    for (int y = 0; y < h; ++y) {
                        auto *pixels = image.scanLine(y);
                        for (int x = 0; x < w; ++x)
                            pixels[x] = uint8_t((maximumLuma - pixels[x]) * 255 / range);
                    }
                }
            } else if (neutralAdaptive) {
                // Erase chromatic video bands and accent focus outlines to pure
                // white, invert neutral text (including dim muted detail rows)
                // onto a clean white canvas, and pair with Sauvola local
                // thresholding so both unfocused and focused rows read cleanly.
                const QImage source = image.convertToFormat(QImage::Format_RGB32);
                const int w = source.width();
                const int h = source.height();
                const int maxDim = std::max(w, h);
                scale = maxDim < 960 ? 4 : (maxDim < 2000 ? 2 : 1);
                image = QImage(w, h, QImage::Format_Grayscale8);
                for (int y = 0; y < h; ++y) {
                    const auto *sourcePixels = reinterpret_cast<const QRgb *>(source.constScanLine(y));
                    auto *pixels = image.scanLine(y);
                    for (int x = 0; x < w; ++x) {
                        const QRgb pixel = sourcePixels[x];
                        const int r = qRed(pixel);
                        const int g = qGreen(pixel);
                        const int b = qBlue(pixel);
                        if (std::max({ r, g, b }) - std::min({ r, g, b }) > 48) {
                            pixels[x] = 255;
                        } else {
                            const int luma = (77 * r + 150 * g + 29 * b + 128) >> 8;
                            const int scaled = (160 - luma) * 255 / (160 - 32);
                            pixels[x] = uint8_t(std::clamp(scaled, 0, 255));
                        }
                    }
                }
                // Strip long straight axis-aligned rules (e.g. TV-mode neutral
                // white focus frames around buttons like Close) that invert to
                // solid closed black boxes and cause Tesseract to drop short
                // enclosed captions on both sparse and block segmentation.
                const int minRuleLength = std::max(44, maxDim / 50);
                std::vector<uint8_t> eraseRule(size_t(w) * size_t(h), 0);
                for (int y = 0; y < h; ++y) {
                    const auto *row = image.constScanLine(y);
                    const size_t rowOffset = size_t(y) * size_t(w);
                    int x = 0;
                    while (x < w) {
                        while (x < w && row[x] >= 160)
                            ++x;
                        const int start = x;
                        while (x < w && row[x] < 160)
                            ++x;
                        if (x - start >= minRuleLength) {
                            for (int k = start; k < x; ++k)
                                eraseRule[rowOffset + size_t(k)] = 1;
                        }
                    }
                }
                for (int x = 0; x < w; ++x) {
                    int y = 0;
                    while (y < h) {
                        while (y < h && image.constScanLine(y)[x] >= 160)
                            ++y;
                        const int start = y;
                        while (y < h && image.constScanLine(y)[x] < 160)
                            ++y;
                        if (y - start >= minRuleLength) {
                            for (int k = start; k < y; ++k)
                                eraseRule[size_t(k) * size_t(w) + size_t(x)] = 1;
                        }
                    }
                }
                for (int y = 0; y < h; ++y) {
                    auto *row = image.scanLine(y);
                    const size_t rowOffset = size_t(y) * size_t(w);
                    for (int x = 0; x < w; ++x) {
                        if (!eraseRule[rowOffset + size_t(x)])
                            continue;
                        row[x] = 255;
                        if (y > 0)
                            image.scanLine(y - 1)[x] = 255;
                        if (y + 1 < h)
                            image.scanLine(y + 1)[x] = 255;
                        if (x > 0)
                            row[x - 1] = 255;
                        if (x + 1 < w)
                            row[x + 1] = 255;
                    }
                }
            } else if (highContrast) {
                // Focused rows can defeat page segmentation even after scaling.
                // Preserve their actual light glyphs against a binary background.
                image = image.convertToFormat(QImage::Format_Grayscale8);
                for (int y = 0; y < image.height(); ++y) {
                    auto *pixels = image.scanLine(y);
                    for (int x = 0; x < image.width(); ++x)
                        pixels[x] = pixels[x] > 64 ? 0 : 255;
                }
            }
            inputPath += adaptiveContrast ? QStringLiteral(".ocr-adaptive.png")
                : positiveContrast        ? QStringLiteral(".ocr-positive.png")
                : neutralAdaptive         ? QStringLiteral(".ocr-neutral-adaptive.png")
                : smoothInverse           ? QStringLiteral(".ocr-smooth-inverse.png")
                : highContrast            ? QStringLiteral(".ocr-contrast.png")
                : neutralText             ? QStringLiteral(".ocr-neutral.png")
                                          : QStringLiteral(".ocr.png");
            // Upscale smooth grayscale passes 2x with a separable Catmull-Rom
            // kernel instead of Qt's bilinear filter, which smears 1px strokes
            // at 8-9px glyph heights on compact mobile viewports.
            const auto catmullUpscale2x = [](const QImage& mapped) {
                const int w = mapped.width();
                const int h = mapped.height();
                // Pixel-center-aligned 2x Catmull-Rom (t = +-0.25 => [-3, 29, 111, -9] / 128).
                const auto catmullQuarter = [](int far, int near, int center, int opposite) -> uint8_t {
                    return uint8_t(std::clamp((-3 * far + 29 * near + 111 * center - 9 * opposite + 64) >> 7, 0, 255));
                };
                QImage horiz(w * 2, h, QImage::Format_Grayscale8);
                for (int y = 0; y < h; ++y) {
                    const auto *src = mapped.constScanLine(y);
                    auto *dst = horiz.scanLine(y);
                    for (int x = 0; x < w; ++x) {
                        const int xm2 = std::max(0, x - 2);
                        const int xm1 = std::max(0, x - 1);
                        const int xp1 = std::min(w - 1, x + 1);
                        const int xp2 = std::min(w - 1, x + 2);
                        dst[2 * x] = catmullQuarter(src[xm2], src[xm1], src[x], src[xp1]);
                        dst[2 * x + 1] = catmullQuarter(src[xp2], src[xp1], src[x], src[xm1]);
                    }
                }
                QImage upscaled(w * 2, h * 2, QImage::Format_Grayscale8);
                for (int y = 0; y < h; ++y) {
                    const auto *rm2 = horiz.constScanLine(std::max(0, y - 2));
                    const auto *rm1 = horiz.constScanLine(std::max(0, y - 1));
                    const auto *rc = horiz.constScanLine(y);
                    const auto *rp1 = horiz.constScanLine(std::min(h - 1, y + 1));
                    const auto *rp2 = horiz.constScanLine(std::min(h - 1, y + 2));
                    auto *even = upscaled.scanLine(2 * y);
                    auto *odd = upscaled.scanLine(2 * y + 1);
                    for (int x = 0; x < w * 2; ++x) {
                        even[x] = catmullQuarter(rm2[x], rm1[x], rc[x], rp1[x]);
                        odd[x] = catmullQuarter(rp2[x], rp1[x], rc[x], rm1[x]);
                    }
                }
                return upscaled;
            };
            const auto catmullUpscale4x = [](const QImage& mapped) {
                const int w = mapped.width();
                const int h = mapped.height();
                // Pixel-center-aligned 4x Catmull-Rom:
                // t = 3/8 => [-45, 399, 745, -75] / 1024; t = 1/8 => [-7, 93, 987, -49] / 1024.
                const auto catmullThreeEighths = [](int far, int near, int center, int opposite) -> uint8_t {
                    return uint8_t(
                        std::clamp((-45 * far + 399 * near + 745 * center - 75 * opposite + 512) >> 10, 0, 255));
                };
                const auto catmullOneEighth = [](int far, int near, int center, int opposite) -> uint8_t {
                    return uint8_t(
                        std::clamp((-7 * far + 93 * near + 987 * center - 49 * opposite + 512) >> 10, 0, 255));
                };
                QImage horiz(w * 4, h, QImage::Format_Grayscale8);
                for (int y = 0; y < h; ++y) {
                    const auto *src = mapped.constScanLine(y);
                    auto *dst = horiz.scanLine(y);
                    for (int x = 0; x < w; ++x) {
                        const int xm2 = std::max(0, x - 2);
                        const int xm1 = std::max(0, x - 1);
                        const int xp1 = std::min(w - 1, x + 1);
                        const int xp2 = std::min(w - 1, x + 2);
                        dst[4 * x] = catmullThreeEighths(src[xm2], src[xm1], src[x], src[xp1]);
                        dst[4 * x + 1] = catmullOneEighth(src[xm2], src[xm1], src[x], src[xp1]);
                        dst[4 * x + 2] = catmullOneEighth(src[xp2], src[xp1], src[x], src[xm1]);
                        dst[4 * x + 3] = catmullThreeEighths(src[xp2], src[xp1], src[x], src[xm1]);
                    }
                }
                QImage upscaled(w * 4, h * 4, QImage::Format_Grayscale8);
                for (int y = 0; y < h; ++y) {
                    const auto *rm2 = horiz.constScanLine(std::max(0, y - 2));
                    const auto *rm1 = horiz.constScanLine(std::max(0, y - 1));
                    const auto *rc = horiz.constScanLine(y);
                    const auto *rp1 = horiz.constScanLine(std::min(h - 1, y + 1));
                    const auto *rp2 = horiz.constScanLine(std::min(h - 1, y + 2));
                    auto *row0 = upscaled.scanLine(4 * y);
                    auto *row1 = upscaled.scanLine(4 * y + 1);
                    auto *row2 = upscaled.scanLine(4 * y + 2);
                    auto *row3 = upscaled.scanLine(4 * y + 3);
                    for (int x = 0; x < w * 4; ++x) {
                        row0[x] = catmullThreeEighths(rm2[x], rm1[x], rc[x], rp1[x]);
                        row1[x] = catmullOneEighth(rm2[x], rm1[x], rc[x], rp1[x]);
                        row2[x] = catmullOneEighth(rp2[x], rp1[x], rc[x], rm1[x]);
                        row3[x] = catmullThreeEighths(rp2[x], rp1[x], rc[x], rm1[x]);
                    }
                }
                return upscaled;
            };
            QSaveFile file(inputPath);
            QImage output = (smoothInverse || neutralAdaptive || positiveContrast)
                ? (scale == 4          ? catmullUpscale4x(image)
                          : scale == 2 ? catmullUpscale2x(image)
                                       : image)
                : (scale == 1 ? image
                              : image.scaled(image.width() * scale, image.height() * scale, Qt::IgnoreAspectRatio,
                                    Qt::SmoothTransformation));
            if (linePadding) {
                QImage padded(
                    output.width() + 2 * linePadding, output.height() + 2 * linePadding, QImage::Format_RGB32);
                padded.fill(Qt::white);
                QPainter painter(&padded);
                painter.drawImage(linePadding, linePadding, output);
                painter.end();
                output = std::move(padded);
            }
            require(file.open(QIODevice::WriteOnly)
                    && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                    && output.save(&file, "PNG") && file.commit(),
                "captured OCR image could not be normalized");
        }
        QStringList arguments { inputPath, "stdout", "-l", "eng", "--psm", QString::number(mode) };
        if (adaptiveContrast || neutralAdaptive)
            arguments << "-c" << "thresholding_method=2";
        arguments << "tsv";
        const QByteArray tsv = runOcr(ocr, arguments, environment);
        QList<Words> lines;
        QString previous;
        for (const QByteArray& row : tsv.split('\n')) {
            const auto fields = row.split('\t');
            if (fields.size() < 12 || fields[0] != "5" || fields[11].trimmed().isEmpty())
                continue;
            const QString key = QString::fromLatin1(fields[1] + ':' + fields[2] + ':' + fields[3] + ':' + fields[4]);
            const qreal combinedScale = scale * sourceScale;
            const QRect bounds = QRectF((fields[6].toDouble() - linePadding) / combinedScale + origin.x(),
                (fields[7].toDouble() - linePadding) / combinedScale + origin.y(), fields[8].toDouble() / combinedScale,
                fields[9].toDouble() / combinedScale)
                                     .toAlignedRect();
            if (key != previous) {
                lines.append(Words {});
                previous = key;
            }
            auto& line = lines.last();
            if (!line.text.isEmpty())
                line.text += ' ';
            line.starts.append(line.text.size());
            line.rectangles.append(bounds);
            line.text += QString::fromUtf8(fields[11]).trimmed();
        }
        return lines;
    }
    QRect label(const QString& text, bool scroll = false, TextSurface surface = TextSurface::Window)
    {
        QRect found;
        QRect previous;
        QString lastObserved;
        const bool primaryCaption = surface == TextSurface::DetailPrimaryCaption;
        const bool singleLine
            = primaryCaption || surface == TextSurface::PlayerRepeatDetail || surface == TextSurface::PlayerSleepDetail;
        const std::string failure = "expected user-visible label was not rendered: " + text.toStdString();
        const auto summarize = [](const QList<Words>& lines) {
            QString summary;
            for (const auto& line : lines) {
                if (line.text.isEmpty() || line.text.contains(QStringLiteral("://")))
                    continue;
                QString clean;
                for (const QChar ch : line.text) {
                    if (ch.isLetterOrNumber() || ch == QLatin1Char(' ') || ch == QLatin1Char('-')
                        || ch == QLatin1Char(':'))
                        clean += ch;
                }
                clean = clean.simplified();
                if (clean.isEmpty())
                    continue;
                if (!summary.isEmpty())
                    summary += QStringLiteral("; ");
                summary += clean;
                if (summary.size() >= 110) {
                    summary.truncate(110);
                    break;
                }
            }
            return summary;
        };
        const auto locate = [&](const QList<Words>& lines) {
            for (const auto& line : lines)
                if (line.text.compare(text, Qt::CaseInsensitive) == 0)
                    return line.phraseBounds(text);
            for (const auto& line : lines) {
                const QRect bounds = line.phraseBounds(text);
                if (!bounds.isEmpty())
                    return bounds;
            }
            for (qsizetype index = 0; index + 1 < lines.size(); ++index) {
                const auto& first = lines[index];
                const auto& second = lines[index + 1];
                if (first.rectangles.isEmpty() || second.rectangles.isEmpty())
                    continue;
                QRect firstBox;
                for (const QRect& rect : first.rectangles)
                    firstBox = firstBox.united(rect);
                QRect secondBox;
                for (const QRect& rect : second.rectangles)
                    secondBox = secondBox.united(rect);
                const int maxHeight = std::max(firstBox.height(), secondBox.height());
                if (secondBox.top() < firstBox.top() || secondBox.top() - firstBox.bottom() > maxHeight * 2)
                    continue;
                Words combined = first;
                const qsizetype offset = combined.text.size() + 1;
                combined.text += QLatin1Char(' ') + second.text;
                for (qsizetype w = 0; w < second.starts.size(); ++w) {
                    combined.starts.append(offset + second.starts[w]);
                    combined.rectangles.append(second.rectangles[w]);
                }
                const QRect bounds = combined.phraseBounds(text);
                if (!bounds.isEmpty())
                    return bounds;
            }
            return QRect {};
        };
        try {
            await(
                [&] {
                    const QImage frame = capture();
                    QString imagePath = screenshotPath;
                    QRect region;
                    if (surface != TextSurface::Window) {
                        const QString key = surface == TextSurface::LibraryRow ? QStringLiteral("libraryRow")
                            : surface == TextSurface::RelatedCard              ? QStringLiteral("relatedCard")
                            : primaryCaption                                   ? QStringLiteral("detailPrimaryCaption")
                            : surface == TextSurface::PlayerRepeatDetail       ? QStringLiteral("playerRepeatDetail")
                            : surface == TextSurface::PlayerSleepDetail        ? QStringLiteral("playerSleepDetail")
                                                                               : QStringLiteral("playerOptions");
                        const auto row = capturedState["visual"].toObject()[key].toObject();
                        const double ratio = capturedState["window"].toObject()["devicePixelRatio"].toDouble(1);
                        region = QRectF(row["x"].toDouble() * ratio, row["y"].toDouble() * ratio,
                            row["width"].toDouble() * ratio, row["height"].toDouble() * ratio)
                                     .toAlignedRect()
                                     .intersected(frame.rect());
                        if (region.isEmpty()) {
                            if (scroll)
                                command({ "key", "down" });
                            return false;
                        }
                        if (singleLine)
                            region = region.adjusted(-3, 0, 4, 0).intersected(frame.rect());
                        imagePath = surface == TextSurface::LibraryRow
                            ? libraryTextPath()
                            : screenshotPath + QStringLiteral(".caption.png");
                        QSaveFile file(imagePath);
                        require(file.open(QIODevice::WriteOnly)
                                && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                                && frame.copy(region).save(&file, "PNG") && file.commit(),
                            "actual rendered region could not be read for caption navigation");
                    }
                    if (singleLine) {
                        const auto preprocess
                            = primaryCaption ? TextPreprocess::Positive : TextPreprocess::SmoothInverse;
                        const auto lines = words(imagePath, 7, region.topLeft(), preprocess);
                        lastObserved = summarize(lines);
                        found = locate(lines);
                        if (found.isEmpty())
                            found = locate(words(imagePath, 7, region.topLeft()));
                        return !found.isEmpty();
                    }
                    const QList<Words> primaryLines
                        = words(imagePath, surface == TextSurface::Window ? 11 : 6, region.topLeft());
                    lastObserved = summarize(primaryLines);
                    QRect candidate = locate(primaryLines);
                    // Sparse-page segmentation can omit text touching a focus outline.
                    // Read the same real frame as a text block rather than accepting a
                    // larger heading's unrelated hit region or manufacturing a label.
                    if (candidate.isEmpty() && surface == TextSurface::Window)
                        candidate = locate(words(imagePath, 6));
                    if (candidate.isEmpty())
                        candidate = locate(words(imagePath, 6, region.topLeft(), TextPreprocess::Neutral));
                    if (candidate.isEmpty())
                        candidate = locate(words(imagePath, 6, region.topLeft(), TextPreprocess::Inverse));
                    if (candidate.isEmpty())
                        candidate = locate(words(imagePath, 11, region.topLeft(), TextPreprocess::SmoothInverse));
                    if (candidate.isEmpty())
                        candidate = locate(words(imagePath, 6, region.topLeft(), TextPreprocess::SmoothInverse));
                    if (candidate.isEmpty())
                        candidate = locate(words(imagePath, 11, region.topLeft(), TextPreprocess::NeutralAdaptive));
                    if (candidate.isEmpty())
                        candidate = locate(words(imagePath, 6, region.topLeft(), TextPreprocess::NeutralAdaptive));
                    if (candidate.isEmpty()) {
                        const QList<Words> positiveLines
                            = words(imagePath, 11, region.topLeft(), TextPreprocess::Positive);
                        candidate = locate(positiveLines);
                        if (candidate.isEmpty())
                            candidate = locate(words(imagePath, 6, region.topLeft(), TextPreprocess::Positive));
                        if (candidate.isEmpty()) {
                            const QString pos = summarize(positiveLines);
                            if (!pos.isEmpty()) {
                                const QString combined
                                    = QStringLiteral("pos: ") + pos + QStringLiteral("; ") + lastObserved;
                                lastObserved = combined.left(110);
                            }
                        }
                    }
                    if (candidate.isEmpty())
                        candidate = locate(words(imagePath, 11, region.topLeft(), TextPreprocess::Adaptive));
                    if (candidate.isEmpty())
                        candidate = locate(words(imagePath, 3, region.topLeft(), TextPreprocess::Adaptive));
                    if (!candidate.isEmpty()) {
                        if (surface != TextSurface::LibraryRow
                            || (!previous.isEmpty() && (candidate.center() - previous.center()).manhattanLength() <= 2
                                && std::abs(candidate.width() - previous.width()) <= 2
                                && std::abs(candidate.height() - previous.height()) <= 2)) {
                            found = candidate;
                            return true;
                        }
                        previous = candidate;
                        return false;
                    }
                    previous = {};
                    if (scroll)
                        command({ "key", "down" });
                    return false;
                },
                failure.c_str(), scroll ? 90000 : 30000);
        } catch (const std::runtime_error& error) {
            if (!lastObserved.isEmpty())
                throw std::runtime_error(std::string(error.what()) + " (observed: " + lastObserved.toStdString() + ")");
            throw;
        }
        return found;
    }
    void click(const QString& text, bool scroll = false, bool right = false, TextSurface surface = TextSurface::Window)
    {
        tap(label(text, scroll, surface), right);
    }
    void openLibrary(const QString& title)
    {
        tap(label(title, false, TextSurface::LibraryRow));
        await(
            [&] {
                const auto current = state();
                return current["route"] == "libraryGrid" && !current["browseLoading"].toBool();
            },
            "actual library-card activation did not navigate to its browse page");
    }
    void openPlaybackSettings()
    {
        if (qEnvironmentVariable("SPOOL_E2E_ANDROID_TV") == "1") {
            // Use the real on-screen gear after revealing the controls.
            QRect button;
            await(
                [&] {
                    const auto observed = command({ "key", "up" });
                    const auto bounds = observed["visual"].toObject()["playerSettings"].toObject();
                    const double ratio = observed["window"].toObject()["devicePixelRatio"].toDouble(1);
                    button = QRectF(bounds["x"].toDouble() * ratio, bounds["y"].toDouble() * ratio,
                        bounds["width"].toDouble() * ratio, bounds["height"].toDouble() * ratio)
                                 .toAlignedRect();
                    return !button.isEmpty();
                },
                "Android TV transport controls did not reveal the Playback settings button");
            tap(button);
            return;
        }
        // Up saturates at Back, Down reaches the timeline and the actions,
        // and Right saturates at Playback settings.
        for (int step = 0; step < 3; ++step)
            command({ "key", "up" });
        command({ "key", "down" });
        command({ "key", "down" });
        for (int step = 0; step < 12; ++step)
            command({ "key", "right" });
        command({ "key", "ok" });
    }
    void selectAndroidTvRepeatCurrent()
    {
        require(device != nullptr, "Android TV remote input requires an owned device");
        const auto waitForFocus = [this](int expected, const char *message) {
            await(
                [&] {
                    const auto menu = state()["visual"].toObject()["playerOptions"].toObject();
                    return menu["repeatFocusIndex"].toInt(-1) == expected;
                },
                message);
        };
        waitForFocus(0, "Android TV Repeat menu did not focus Off");
        device->androidTvRemoteKey("KEYCODE_DPAD_DOWN");
        waitForFocus(1, "Android TV remote did not focus Repeat queue");
        device->androidTvRemoteKey("KEYCODE_DPAD_DOWN");
        waitForFocus(2, "Android TV remote did not focus Repeat current item");
        device->androidTvRemoteKey("KEYCODE_DPAD_CENTER");
        await([&] { return !state()["visual"].toObject().contains("playerOptions"); },
            "Android TV remote did not accept the focused repeat option");
        openPlaybackSettings();
        label("Current item", false, TextSurface::PlayerRepeatDetail);
        device->androidTvRemoteKey("KEYCODE_BACK");
        await([&] { return !state()["visual"].toObject().contains("playerOptions"); },
            "Android TV remote did not dismiss playback settings");
    }
    void pauseForObservation(const char *message)
    {
        QJsonObject playback;
        int observations = 0;
        int timeouts = 0;
        QElapsedTimer lastObservation;
        lastObservation.start();
        try {
            await(
                [&] {
                    playback = state(true)["playback"].toObject();
                    require(!playback["failed"].toBool(), "playback failed before it could be paused");
                    if (!playback["active"].toBool() || !playback["ready"].toBool())
                        return false;
                    QJsonObject response;
                    const auto paused = command({ "pause" }, {}, false, {}, &response);
                    // Ready is a snapshot, not a lease on a session. A repeat
                    // or renderer replacement can leave a gap between status
                    // and pause; wait for a real ready session again.
                    if (response["error"].toObject()["code"] == "no_playback")
                        return false;
                    require(response["ok"].toBool(), "pause control request was rejected");
                    playback = paused["playback"].toObject();
                    require(!playback["failed"].toBool(), "playback had already failed before pause could settle");
                    return true;
                },
                message);
            await(
                [&] {
                    const auto observed = state(true)["playback"].toObject();
                    if (observed.isEmpty()) {
                        ++timeouts;
                        return false;
                    }
                    ++observations;
                    lastObservation.restart();
                    playback = observed;
                    require(!playback["failed"].toBool(), "playback failed while waiting for pause");
                    return playback["paused"].toBool();
                },
                message);
        } catch (const std::exception& error) {
            const QString sample = device
                ? device->blockedProcessSample()
                : sampleBlockedProcess(app.processId(), QFileInfo(screenshotPath).absolutePath(), environment);
            std::cerr << "app-journey-stack: " << sample.toStdString() << '\n';
            std::cerr << "app-journey-pause: observations=" << observations << ", timeouts=" << timeouts
                      << ", position_sample_age_ms=" << playback["positionSampleAgeMs"].toInteger(-1)
                      << ", mpv_event_age_ms=" << playback["mpvEventAgeMs"].toInteger(-1)
                      << ", pause_request_age_ms=" << playback["pauseRequestAgeMs"].toInteger(-1) << '\n';
            throw std::runtime_error(QStringLiteral(
                "%1 (active=%2, loaded=%3, ready=%4, paused=%5, failed=%6, position=%7, duration=%8, pause=%9, "
                "observations=%10, timeouts=%11, last_observation_ms=%12, stack_symbols=%13, "
                "position_sample_age_ms=%14, mpv_event_age_ms=%15, pause_request_age_ms=%16)")
                    .arg(QString::fromUtf8(error.what()))
                    .arg(playback["active"].toBool())
                    .arg(playback["loaded"].toBool())
                    .arg(playback["ready"].toBool())
                    .arg(playback["paused"].toBool())
                    .arg(playback["failed"].toBool())
                    .arg(playback["position"].toDouble(), 0, 'f', 2)
                    .arg(playback["duration"].toDouble(), 0, 'f', 2)
                    .arg(playback["pauseCommand"].toString())
                    .arg(observations)
                    .arg(timeouts)
                    .arg(lastObservation.elapsed())
                    .arg(sample)
                    .arg(playback["positionSampleAgeMs"].toInteger(-1))
                    .arg(playback["mpvEventAgeMs"].toInteger(-1))
                    .arg(playback["pauseRequestAgeMs"].toInteger(-1))
                    .toStdString());
        }
    }
    void await(const std::function<bool()>& predicate, const char *message, int timeout = 30000)
    {
        QElapsedTimer clock;
        clock.start();
        do {
            ensureRunning();
            try {
                if (predicate())
                    return;
            } catch (const std::exception& error) {
                throw std::runtime_error(std::string(message) + ": " + error.what());
            }
            turn();
        } while (clock.elapsed() < timeout);
        throw std::runtime_error(message);
    }
    QProcess app;

private:
    QString libraryTextPath() const
    {
        return QFileInfo(screenshotPath).dir().filePath("library-text.png");
    }
    void tap(const QRect& bounds, bool right = false)
    {
        const auto window = state()["window"].toObject();
        const double ratio = window["devicePixelRatio"].toDouble(1);
        const QString x = QString::number(bounds.center().x() / ratio);
        const QString y = QString::number(bounds.center().y() / ratio);
        if (right) {
            command({ "pointer", "right-click", x, y });
        } else {
            command({ "pointer", "move", x, y });
            turn(50);
            command({ "pointer", "press", x, y });
            turn(50);
            command({ "pointer", "release", x, y });
        }
        turn(200);
    }
    void ensureRunning() const
    {
        if (!device && app.state() == QProcess::NotRunning) {
            if (app.exitStatus() == QProcess::CrashExit) {
                std::cerr << "app-journey: actual product crashed; private artifacts retained at "
                          << QFileInfo(screenshotPath).absolutePath().toStdString() << std::endl;
                SpoolTests::propagateCrash(app.exitCode());
            }
            throw std::runtime_error(QStringLiteral("built Spool exited during the journey (code=%1, status=normal)")
                    .arg(app.exitCode())
                    .toStdString());
        }
    }
    QString control;
    QProcessEnvironment environment;
    QString screenshotPath;
    QString ocr;
    QJsonObject capturedState;
    std::unique_ptr<DeviceSession> device;
    const QString instance = QStringLiteral("app-journey-%1").arg(QCoreApplication::applicationPid());
};

bool dominant(const QColor& color, int channel)
{
    const int values[] { color.red(), color.green(), color.blue() };
    return values[channel] > 70 && values[channel] > values[(channel + 1) % 3] * 1.6
        && values[channel] > values[(channel + 2) % 3] * 1.6;
}
bool frameHasBands(const QImage& image, int upperChannel)
{
    // Interior regions avoid letterboxing, chrome, captions and antialiasing.
    // Absolute RGB equality is intentionally not a renderer contract.
    const int videoWidth = std::min(image.width(), image.height() * 16 / 9);
    const int videoHeight = videoWidth * 9 / 16;
    const int upperY = image.height() / 2 - videoHeight / 4;
    int upper = 0, lower = 0, samples = 0;
    for (int y = upperY - videoHeight / 16; y < upperY + videoHeight / 16; y += 4) {
        for (int x = image.width() / 2 - videoWidth / 8; x < image.width() / 2 + videoWidth / 8; x += 4) {
            upper += dominant(image.pixelColor(x, y), upperChannel);
            lower += dominant(image.pixelColor(x, y + videoHeight / 2), 2);
            ++samples;
        }
    }
    return samples > 0 && upper > samples * 0.7 && lower > samples * 0.7;
}
bool posterVisible(const QImage& image)
{
    int orange = 0, magenta = 0;
    for (int y = 0; y < image.height(); y += 4)
        for (int x = 0; x < image.width(); x += 4) {
            const QColor c = image.pixelColor(x, y);
            orange += c.red() > 150 && c.green() > 60 && c.green() < c.red() * 0.85 && c.blue() < 70;
            magenta += c.red() > 100 && c.blue() > 100 && c.green() < std::min(c.red(), c.blue()) * 0.5;
        }
    return orange > 100 && magenta > 100;
}
QByteArray media(const QString& directory, const QProcessEnvironment& environment)
{
    const QString path = directory + "/journey.mkv";
    const QString ffmpeg = executable("SPOOL_E2E_FFMPEG", QStandardPaths::findExecutable("ffmpeg"));
    run(ffmpeg,
        { "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
            "color=c=blue:s=640x360:r=10:d=90", "-vf",
            "drawbox=x=0:y=0:w=640:h=180:color=red:t=fill:enable='lt(t,12)',"
            "drawbox=x=0:y=0:w=640:h=180:color=lime:t=fill:enable='gte(t,12)'",
            "-an", "-c:v", "ffv1", "-level", "3", "-g", "1", "-threads", "1", path },
        environment, {}, 60000);
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "finite media fixture was not generated");
    const QByteArray bytes = file.readAll();
    require(bytes.size() > 1024, "finite media fixture is empty");
    return bytes;
}
QProcessEnvironment isolatedEnvironment(const QString& directory)
{
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const QString& key : environment.keys()) {
        if (key.startsWith("SPOOL_") && key != "SPOOL_E2E_ISOLATED_DISPLAY" && !key.startsWith("SPOOL_E2E_")
            && key != "SPOOL_GL_BACKEND" && key != "SPOOL_RENDER_API")
            environment.remove(key);
    }
    for (const auto& pair :
        { std::pair { "SPOOL_DATA_HOME", "app-data" }, { "SPOOL_CREDENTIAL_STORE_DIR", "credentials" } }) {
        const QString path = directory + '/' + pair.second;
        require(QDir().mkpath(path), "isolated app directory could not be created");
        environment.insert(pair.first, path);
    }
    // Reuse the version-isolated native launcher convention. QMAKEPATH
    // supplies split Qt module roots in Nix shells, but QQmlEngine does not
    // turn those development roots into runtime import paths on its own.
    if (!environment.value("QMAKEPATH").isEmpty()) {
        const QString version = QString::fromLatin1(qVersion());
        const QChar separator = QDir::listSeparator();
        QStringList qmlPaths;
        QStringList pluginPaths;
        const auto append = [](QStringList& paths, const QString& path) {
            if (!path.isEmpty() && QDir(path).exists() && !paths.contains(path))
                paths.append(path);
        };
        const auto sameVersion
            = [&](const QString& path) { return path.contains('-' + version + '/') || path.endsWith('-' + version); };
        append(qmlPaths, QLibraryInfo::path(QLibraryInfo::QmlImportsPath));
        append(pluginPaths, QLibraryInfo::path(QLibraryInfo::PluginsPath));
        for (const QString& root : environment.value("QMAKEPATH").split(separator, Qt::SkipEmptyParts)) {
            if (sameVersion(root)) {
                append(qmlPaths, QDir(root).filePath("lib/qt-6/qml"));
                append(pluginPaths, QDir(root).filePath("lib/qt-6/plugins"));
            }
        }
        for (const QString& path :
            environment.value("NIXPKGS_QT6_QML_IMPORT_PATH").split(separator, Qt::SkipEmptyParts))
            if (sameVersion(path))
                append(qmlPaths, path);
        for (const QString& path : environment.value("QT_PLUGIN_PATH").split(separator, Qt::SkipEmptyParts))
            if (sameVersion(path))
                append(pluginPaths, path);
        if (!qmlPaths.isEmpty()) {
            environment.insert("QML2_IMPORT_PATH", qmlPaths.join(separator));
            environment.insert("QML_IMPORT_PATH", qmlPaths.join(separator));
            environment.insert("NIXPKGS_QT6_QML_IMPORT_PATH", qmlPaths.join(separator));
        }
        if (!pluginPaths.isEmpty())
            environment.insert("QT_PLUGIN_PATH", pluginPaths.join(separator));
    }
    environment.insert("QT_LOGGING_RULES", "*.debug=false");
    environment.insert("LC_ALL", "C.UTF-8");
    environment.insert("LANG", "C.UTF-8");
    return environment;
}
}

SPOOL_TEST_MAIN("app-journey")
{
    QCoreApplication application(argc, argv);
    const QString artifactBase
        = qEnvironmentVariable("SPOOL_E2E_ARTIFACT_DIR", QDir::current().absoluteFilePath("test-artifacts"));
    QDir().mkpath(artifactBase);
    QTemporaryDir root(QDir(artifactBase).filePath("app-journey-XXXXXX"));
#if defined(Q_OS_MACOS)
    std::optional<QTemporaryDir> runtime;
#endif
    QString fixturePhase = QStringLiteral("login");
    try {
        QString deviceKind;
        const QStringList arguments = application.arguments().mid(1);
        for (qsizetype index = 0; index < arguments.size(); ++index) {
            if (arguments[index] == "--device") {
                require(index + 1 < arguments.size(), "--device requires android or tvos");
                deviceKind = arguments[++index];
                require(deviceKind == "android" || deviceKind == "tvos" || deviceKind == "ios",
                    "--device requires android, tvos or ios");
            }
        }
#ifdef SPOOL_DEVICE_JOURNEY_ONLY
        require(!deviceKind.isEmpty(), "device journey controller requires an isolated device");
#endif
        if (deviceKind.isEmpty()) {
            require(qEnvironmentVariable("SPOOL_E2E_ISOLATED_DISPLAY") == "1",
                "app-journey refuses a user desktop: driver must supply an isolated GPU display");
            const QString platform = qEnvironmentVariable("QT_QPA_PLATFORM").section(':', 0, 0);
            require(platform != "offscreen" && platform != "minimal"
                    && qEnvironmentVariable("QT_QUICK_BACKEND") != "software"
                    && qEnvironmentVariable("QSG_RHI_BACKEND") != "software",
                "app-journey requires a real GPU backend, not Qt offscreen/software");
        }
        require(root.isValid(), "journey temporary directory could not be created");
        auto environment = isolatedEnvironment(root.path());
#if defined(Q_OS_MACOS)
        if (deviceKind.isEmpty()) {
            runtime.emplace(QStringLiteral("/tmp/spool-runtime-XXXXXX"));
            require(runtime->isValid(), "private macOS runtime directory could not be created");
            environment.insert("XDG_RUNTIME_DIR", runtime->path());
        }
#endif
#ifdef SPOOL_E2E_APP
        const QString appDefault = QStringLiteral(SPOOL_E2E_APP);
#else
        const QString appDefault;
#endif
#ifdef SPOOL_E2E_SPOOLET
        const QString controlDefault = QStringLiteral(SPOOL_E2E_SPOOLET);
#else
        const QString controlDefault;
#endif
        const QString app = deviceKind.isEmpty() ? executable("SPOOL_E2E_APP", appDefault) : QString();
        const QString control = executable("SPOOL_E2E_SPOOLET", controlDefault);
        AppJourney::JellyfinFixture fixture(media(root.path(), environment));
        const auto retainFixtureCounters = qScopeGuard([&] {
            const QJsonObject counters { { "format", 1 }, { "phase", fixturePhase },
                { "successfulLogins", fixture.successfulLogins }, { "rejectedLogins", fixture.rejectedLogins },
                { "authenticatedViews", fixture.authenticatedViews },
                { "authenticatedBrowse", fixture.authenticatedBrowse },
                { "playedFilterRequests", fixture.playedFilterRequests },
                { "authenticatedRequests", fixture.authenticatedRequests },
                { "playbackNegotiations", fixture.playbackNegotiations }, { "mediaRequests", fixture.mediaRequests },
                { "mediaBytes", fixture.mediaBytes }, { "detailRequests", fixture.detailRequests },
                { "detailResponses", fixture.detailResponses }, { "episodeRequests", fixture.episodeRequests },
                { "episodeDetailRequests", fixture.episodeDetailRequests },
                { "episodeDetailResponses", fixture.episodeDetailResponses },
                { "playedMutations", fixture.playedItems.size() }, { "playbackReports", fixture.reports.size() },
                { "unexpectedRequests", fixture.unexpected.size() } };
            QSaveFile diagnostics(root.filePath("fixture-counters.json"));
            const QByteArray bytes = QJsonDocument(counters).toJson();
            const bool saved = diagnostics.open(QIODevice::WriteOnly)
                && diagnostics.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                && diagnostics.write(bytes) == bytes.size() && diagnostics.commit();
            if (!saved)
                std::cerr << "app-journey: private fixture counter receipt could not be retained\n";
        });
        const auto retainFixtureRejections = qScopeGuard([&] {
            if (fixture.unexpected.isEmpty())
                return;
            // Retain rejection categories/paths even on an earlier exception,
            // never request headers, bodies, credentials, or query parameters.
            QSaveFile diagnostics(root.filePath("fixture-rejections.json"));
            const auto bytes = QJsonDocument(QJsonArray::fromStringList(fixture.unexpected)).toJson();
            const bool saved = diagnostics.open(QIODevice::WriteOnly)
                && diagnostics.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                && diagnostics.write(bytes) == bytes.size() && diagnostics.commit();
            if (!saved)
                std::cerr << "app-journey: private fixture rejection receipt could not be retained\n";
        });
        require(fixture.listening(), "isolated loopback fixture could not listen");
        Journey journey(control, environment, root.path());
        journey.launch(app, fixture.origin(), deviceKind);
        journey.label("Welcome to Spool");
        journey.assertControlBoundaries();
        // The first playback phase checks the saved file and its timeline.
        // Use the inexpensive renderer profile to avoid a quality step-down
        // during startup on a slow CI display. Apply it before sign-in so a
        // settings-sync request cannot cross the offline boundary.
        const QString renderQualityKey = QStringLiteral("playback/renderQuality");
        const QString initialRenderQuality = journey.command({ "settings", "get", renderQualityKey })["values"]
                                                 .toObject()[renderQualityKey]
                                                 .toString();
        require(journey.command({ "settings", "set", renderQualityKey, "\"fast\"" })["value"] == "fast",
            "saved-media renderer profile was not applied");
        const bool television
            = deviceKind == "tvos" || (deviceKind == "android" && qEnvironmentVariable("SPOOL_E2E_ANDROID_TV") == "1");
        journey.click("Jellyfin", true);
        journey.label("Choose a server");
        journey.text(fixture.origin(), television);
        journey.command({ "key", "ok" });
        journey.label("Username");
        journey.text("journey", television);
        journey.command({ "key", "ok" });
        journey.text("incorrect-password", television);
        journey.command({ "key", "ok" });
        try {
            journey.await(
                [&] { return fixture.rejectedLogins == 1; }, "wrong password was not rejected by the real login path");
        } catch (const std::exception&) {
            try {
                journey.capture();
            } catch (const std::exception&) {
            }
            throw std::runtime_error(QStringLiteral("wrong password was not rejected by the real login path "
                                                    "(rejected=%1, successful=%2, unexpected=%3)")
                    .arg(fixture.rejectedLogins)
                    .arg(fixture.successfulLogins)
                    .arg(fixture.unexpected.size())
                    .toStdString());
        }
        journey.label("Wrong username or password");
        journey.text("journey-password", television);
        journey.command({ "key", "ok" });
        journey.await([&] { return fixture.successfulLogins == 1 && fixture.authenticatedViews > 0; },
            "login did not activate an authenticated provider library");
        // A sign-in opens straight into Home: no profile or startup question.
        journey.await([&] { return journey.state()["route"] == "home"; },
            "a finished sign-in did not open the actual Home route by itself");
        journey.openLibrary("Journey Library");
        journey.await([&] { return fixture.authenticatedBrowse > 0 && !journey.state()["browseLoading"].toBool(); },
            "browse did not request the selected library");
        journey.label("Journey Film");
        journey.await([&] { return posterVisible(journey.capture()); },
            "browse title is present but independent fixture artwork is not rendered");
        const auto rows = journey.command({ "items", "browse" })["items"].toArray();
        require(rows.size() == 2, "initial cached library must contain the immutable Movie and Episode");
        const auto movieRow = std::find_if(rows.cbegin(), rows.cend(),
            [](const QJsonValue& row) { return row.toObject()["id"].toString().endsWith(":journey-movie"); });
        const auto episodeRow = std::find_if(rows.cbegin(), rows.cend(),
            [](const QJsonValue& row) { return row.toObject()["id"].toString().endsWith(":journey-episode"); });
        require(movieRow != rows.cend() && (*movieRow).toObject()["title"] == "Journey Film"
                && (*movieRow).toObject()["type"] == "Movie" && (*movieRow).toObject()["playable"].toBool(),
            "visible browse did not expose the original playable Movie");
        require(episodeRow != rows.cend() && (*episodeRow).toObject()["title"] == "Journey Episode"
                && (*episodeRow).toObject()["type"] == "Episode" && (*episodeRow).toObject()["playable"].toBool(),
            "initial browse cache did not expose the distinct playable Episode");
        const QString itemId = (*movieRow).toObject()["id"].toString();
        require(itemId != "journey-movie" && (*episodeRow).toObject()["id"].toString() != "journey-episode",
            "provider items lost account scoping");

        const auto libraryRows = journey.command({ "items", "libraries" })["items"].toArray();
        const auto selectedLibrary = std::find_if(libraryRows.cbegin(), libraryRows.cend(),
            [](const QJsonValue& row) { return row.toObject()["title"] == "Journey Library"; });
        require(selectedLibrary != libraryRows.cend(), "selected library missing from the catalog inventory");
        const auto catalogPage = journey.command({ "catalog", selectedLibrary->toObject()["id"].toString(), "0", "1" });
        require(catalogPage["items"].toArray().size() == 1
                && catalogPage["items"].toArray().first().toObject()["id"].toString() == itemId,
            "direct catalog page did not preserve scoped provider identity or its row limit");
        require(journey.command({ "items", "browse" })["count"].toInt(-1) == 2,
            "catalog inspection changed the visible browse model");

        fixturePhase = QStringLiteral("library-background-refresh");
        const int browseBeforeRefresh = fixture.authenticatedBrowse;
        fixture.setLibraryBrowseHeld(true);
        journey.await([&] { return fixture.sendLibraryChanged(); }, "library event socket did not connect");
        journey.await([&] { return fixture.authenticatedBrowse > browseBeforeRefresh; },
            "library change did not request a refreshed listing");
        require(journey.command({ "items", "browse" })["count"].toInt(-1) == 2,
            "background refresh cleared the visible catalogue while awaiting the server");
        require(posterVisible(journey.capture()), "background refresh blanked the existing library posters");
        fixture.setLibraryBrowseHeld(false);
        journey.await(
            [&] { return !journey.state()["browseLoading"].toBool(); }, "background library refresh did not finish");

        fixturePhase = QStringLiteral("filter-empty-recovery");
        const int filtersBefore = fixture.playedFilterRequests;
        journey.click("Filter");
        journey.click("Status");
        journey.click("Played");
        journey.await(
            [&] {
                return fixture.playedFilterRequests > filtersBefore && !journey.state()["browseLoading"].toBool()
                    && journey.command({ "items", "browse" })["count"].toInt(-1) == 0;
            },
            "Played filter did not reach the provider and replace the library with its empty result");
        require(journey.state()["route"] == "libraryGrid", "empty filtering abandoned the library route");
        journey.click("Reset");
        journey.await(
            [&] {
                return !journey.state()["browseLoading"].toBool()
                    && journey.command({ "items", "browse" })["count"].toInt(-1) == 2;
            },
            "filter reset did not restore the server catalogue");
        journey.command({ "key", "back" });
        require(journey.state()["route"] == "libraryGrid", "Back from filters popped the library");
        // No pointer refocus or explicit `open`: a remote must be able to use
        // the grid immediately after its empty model was replaced.
        journey.command({ "key", "ok" });
        journey.await([&] { return journey.state()["route"] == "itemDetails"; },
            "remote activation after filter reset remained stranded in the dismissed menu");
        journey.label("Journey Film");

        fixturePhase = QStringLiteral("nested-details-return");
        const int nestedDetailsBefore = fixture.episodeDetailResponses;
        journey.label("More Like This", true);
        journey.click("Journey Series", true, false, TextSurface::RelatedCard);
        journey.await([&] { return fixture.episodeDetailResponses > nestedDetailsBefore; },
            "related-item activation did not fetch the selected episode through the real provider");
        journey.label("Journey Episode");
        journey.command({ "key", "back" });
        require(journey.state()["route"] == "itemDetails", "Back from related details skipped the parent details");
        // Return to the parent heading through ordinary remote navigation;
        // its identity must not still be bound to the child details/model.
        for (int step = 0; step < 6; ++step)
            journey.command({ "key", "up" });
        journey.label("Journey Film");
        journey.command({ "key", "back" });
        journey.await([&] { return journey.state()["route"] == "libraryGrid"; },
            "second Back did not return from retained parent details to the originating library");
        journey.command({ "key", "ok" });
        journey.await([&] { return journey.state()["route"] == "itemDetails"; },
            "returning from nested details lost the library's remote selection");
        journey.label("Journey Film");
        journey.command({ "key", "back" });
        journey.await([&] { return journey.state()["route"] == "libraryGrid"; },
            "reopened movie did not return to the library before downloading");

        // Open and operate the real item-menu/download dialog; list is only an observer.
        fixturePhase = QStringLiteral("movie-download");
        journey.click("Journey Film", false, true);
        journey.click("Download");
        // Pointer hover can move the chooser's selection as it opens. Activate
        // the actual rendered Original row, never an assumed initial focus.
        journey.click("Original");
        QString jobId;
        journey.await(
            [&] {
                const auto jobs = journey.command({ "downloads", "list" })["jobs"].toArray();
                if (jobs.size() != 1)
                    return false;
                const auto job = jobs[0].toObject();
                require(job["quality"] == "Original", "actual download chooser did not start the Original file");
                if (job["failed"].toBool()) {
                    bool captured = false;
                    try {
                        const QImage frame = journey.capture();
                        QSaveFile file(root.filePath("download-failure.png"));
                        captured = file.open(QIODevice::WriteOnly)
                            && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                            && frame.save(&file, "PNG") && file.commit();
                    } catch (const std::exception&) {
                        // A missing diagnostic must not replace the observed failed job.
                    }
                    throw std::runtime_error(
                        QStringLiteral("real provider download failed (received=%1, total=%2, fixture_size=%3, "
                                       "server_media_bytes=%4, failure_frame=%5)")
                            .arg(job["received"].toInteger())
                            .arg(job["total"].toInteger())
                            .arg(fixture.mediaSize())
                            .arg(fixture.mediaBytes)
                            .arg(captured)
                            .toStdString());
                }
                jobId = job["id"].toString();
                return job["state"] == "complete" && job["received"].toInteger() == fixture.mediaSize();
            },
            "download did not finish with the exact independent server byte count", 60000);
        require(
            !jobId.isEmpty() && fixture.mediaBytes >= fixture.mediaSize(), "download never fetched the fixture media");
        journey.label("Downloaded");
        journey.click("Close");

        journey.command({ "navigate", "settings" });
        journey.label("Appearance");
        journey.click("Search settings", true);
        journey.text("playback/seekPreviews");
        journey.command({ "key", "ok" });
        journey.click("Seek previews", true);
        const bool originalPreview = journey.command({ "settings", "get", "playback/seekPreviews" })["values"]
                                         .toObject()["playback/seekPreviews"]
                                         .toBool();
        journey.click("Seek previews", true);
        journey.await(
            [&] {
                return journey.command({ "settings", "get", "playback/seekPreviews" })["values"]
                           .toObject()["playback/seekPreviews"]
                           .toBool()
                    != originalPreview;
            },
            "settings UI toggle did not commit its public value");
        journey.command({ "key", "back" });
        journey.command({ "key", "back" });
        journey.label("Appearance");
        journey.click("Downloads", true);
        journey.click("Play saved files offline", true);
        journey.label("Journey Film");
        journey.label("Downloaded");
        QString savedItemId;
        journey.await(
            [&] {
                const auto libraries = journey.command({ "items", "libraries" })["items"].toArray();
                for (const auto& value : libraries) {
                    const auto library = value.toObject();
                    if (library["title"] != "Downloads")
                        continue;
                    const auto files
                        = journey.command({ "catalog", library["id"].toString(), "0", "1" })["items"].toArray();
                    if (files.isEmpty())
                        return false;
                    const auto file = files[0].toObject();
                    require(file["title"] == "Journey Film" && file["playable"].toBool(),
                        "completed download was not published as the playable saved movie");
                    savedItemId = file["id"].toString();
                    return !savedItemId.isEmpty();
                }
                return false;
            },
            "download never appeared in the local catalogue");
        fixturePhase = QStringLiteral("movie-offline");
        // Every new authenticated request is unavailable, including playback
        // negotiation, metadata and media. The pre-existing idle event socket
        // cannot supply a playback plan or a stream.
        fixture.setNewRequestsAvailable(false);
        const int authenticatedBeforeOffline = fixture.authenticatedRequests;
        const int negotiationsBeforeOffline = fixture.playbackNegotiations;
        const int remoteRequests = fixture.mediaRequests;
        // Select the actual completed row, which focuses its Play offline action;
        // activate that focused button through the normal keyboard/remote path.
        journey.click("Journey Film");
        journey.command({ "key", "ok" });
        const auto savedPlayback = [&] {
            const auto playback = journey.state(true)["playback"].toObject();
            const int endReason = playback["mpvEndReason"].toInt(-1);
            if (playback["failed"].toBool() || endReason >= 0) {
                throw std::runtime_error(QStringLiteral("saved movie ended early (failed=%1, reason=%2, error=%3)")
                        .arg(playback["failed"].toBool())
                        .arg(endReason)
                        .arg(playback["mpvEndError"].toInt())
                        .toStdString());
            }
            return playback;
        };
        const auto savedPlaybackCommand = [&](const QStringList& args) {
            QJsonObject response;
            try {
                journey.command(args, {}, true, {}, &response);
            } catch (const std::exception&) {
                if (response["error"].toObject()["code"] == "no_playback")
                    savedPlayback();
                throw;
            }
        };
        journey.await(
            [&] {
                const auto playback = savedPlayback();
                return playback["active"].toBool() && playback["title"] == "Journey Film";
            },
            "the Play offline action did not start the saved movie");
        // Request pause during startup. The player carries the intent into
        // loadfile if the renderer is still being prepared.
        savedPlaybackCommand({ "pause" });
        journey.await(
            [&] {
                const auto playback = savedPlayback();
                return playback["active"].toBool() && playback["loaded"].toBool() && playback["ready"].toBool()
                    && playback["paused"].toBool() && playback["embeddedVideo"].toBool()
                    && playback["duration"].toDouble() > 29;
            },
            "saved movie did not become ready for exact paused-frame seeking");
        savedPlaybackCommand({ "seek", "2" });
        journey.await(
            [&] {
                const auto playback = savedPlayback();
                const double position = playback["position"].toDouble();
                return playback["paused"].toBool() && position > 1.5 && position < 2.5
                    && frameHasBands(journey.capture(true), 0);
            },
            "exact seek to 2 seconds did not render upright red-over-blue bands");
        savedPlaybackCommand({ "seek", "14" });
        journey.await(
            [&] {
                const auto playback = savedPlayback();
                const double position = playback["position"].toDouble();
                return playback["paused"].toBool() && position > 13.5 && position < 14.5
                    && frameHasBands(journey.capture(true), 1);
            },
            "exact seek to 14 seconds did not render upright green-over-blue bands");
        savedPlaybackCommand({ "resume" });
        journey.await(
            [&] {
                const auto playback = savedPlayback();
                return !playback["paused"].toBool() && playback["position"].toDouble() > 14.5;
            },
            "real media clock did not progress after resume");
        journey.pauseForObservation("saved movie did not pause after its clock advanced");
        const double stoppedPosition = savedPlayback()["position"].toDouble();
        require(stoppedPosition > 14.5, "saved movie lost its position before stopping");
        journey.command({ "stop" });
        journey.await(
            [&] { return !journey.state()["playback"].toObject()["active"].toBool(); }, "player did not stop");
        journey.await(
            [&] {
                const auto savedItem = journey.command({ "item-state", savedItemId });
                return !savedItem["played"].toBool()
                    && std::abs(savedItem["positionSeconds"].toDouble() - stoppedPosition) < 0.5;
            },
            "stopping the movie did not save its paused position in the offline library");
        require(fixture.playbackNegotiations == negotiationsBeforeOffline,
            "saved-media playback renegotiated against the unavailable provider");
        require(fixture.mediaRequests == remoteRequests,
            "saved-media playback unexpectedly fetched the unavailable server");
        const int offlineRequests = fixture.authenticatedRequests - authenticatedBeforeOffline;
        if (offlineRequests != 0) {
            throw std::runtime_error(
                QStringLiteral("saved-media playback contacted an unavailable authenticated provider endpoint "
                               "(total=%1, eventReconnect=%2, progress=%3)")
                    .arg(offlineRequests)
                    .arg(fixture.offlineSocketRequests)
                    .arg(fixture.offlineProgressRequests)
                    .toStdString());
        }

        fixture.setNewRequestsAvailable(true);
        require(journey.command({ "settings", "set", renderQualityKey, '"' + initialRenderQuality + '"' })["value"]
                == initialRenderQuality,
            "original renderer profile was not restored");
        // Keep server UserData at zero even after accepted stop reports. The
        // real provider must hydrate details/series rows without rolling back
        // the app's newer episode stop state.
        fixturePhase = QStringLiteral("episode-initial");
        fixture.setEpisodeDetailsHeld(true);
        const int detailsBeforePlayback = fixture.episodeDetailRequests;
        const int episodesBeforePlayback = fixture.episodeRequests;
        const int negotiationsBeforeOnline = fixture.playbackNegotiations;
        const qsizetype reportsBeforeOnline = fixture.reports.size();
        journey.command({ "home" });
        journey.openLibrary("Journey Library");
        journey.click("Journey Series");
        journey.command({ "key", "ok" });
        journey.await(
            [&] {
                // A single read-only observation can miss its client deadline
                // while the new renderer surface starts. Keep the enclosing
                // deadline and require real provider negotiation and playback.
                return fixture.playbackNegotiations > negotiationsBeforeOnline
                    && journey.state(true)["playback"].toObject()["loaded"].toBool();
            },
            "online UI play did not negotiate and load real provider media");
        journey.await(
            [&] {
                return fixture.episodeDetailRequests > detailsBeforePlayback
                    && fixture.episodeRequests > episodesBeforePlayback;
            },
            "episode playback did not use real delayed details and series catalogue requests");
        journey.await(
            [&] {
                return std::any_of(fixture.reports.cbegin() + reportsBeforeOnline, fixture.reports.cend(),
                    [](const QJsonObject& report) {
                        return report["endpoint"] == "/Sessions/Playing" && report["ItemId"] == "journey-episode";
                    });
            },
            "real provider playback start for the online item was not reported");
        journey.pauseForObservation("episode did not pause before the controlled stop position");
        journey.command({ "seek", "14" });
        journey.await(
            [&] {
                const double position = journey.state()["playback"].toObject()["position"].toDouble();
                return position > 13.5 && position < 14.5 && frameHasBands(journey.capture(true), 1);
            },
            "online GPU playback did not deliver negotiated media");
        journey.command({ "stop" });
        fixturePhase = QStringLiteral("episode-first-stop");
        qint64 firstStopTicks = -1;
        journey.await(
            [&] {
                return std::any_of(fixture.reports.cbegin() + reportsBeforeOnline, fixture.reports.cend(),
                    [&firstStopTicks](const QJsonObject& report) {
                        const qint64 ticks = report["PositionTicks"].toVariant().toLongLong();
                        if (report["endpoint"] != "/Sessions/Playing/Stopped" || report["ItemId"] != "journey-episode"
                            || ticks <= 135000000 || ticks >= 145000000)
                            return false;
                        firstStopTicks = ticks;
                        return true;
                    });
            },
            "provider stop receipt did not independently observe the played media position");
        journey.await([&] { return !journey.state()["playback"].toObject()["active"].toBool(); },
            "episode did not finish stopping before delayed details were released");
        const int responsesBeforeRelease = fixture.episodeDetailResponses;
        fixture.setEpisodeDetailsHeld(false);
        journey.await([&] { return fixture.episodeDetailResponses > responsesBeforeRelease; },
            "stale episode detail response was not released");
        // Read the rendered caption without the accent button's focus outline
        // or the neighboring Start from beginning action in the OCR region.
        journey.label("Resume", false, TextSurface::DetailPrimaryCaption);

        const auto resumeEpisode = [&](qint64 expectedTicks) {
            fixturePhase = QStringLiteral("episode-resume");
            journey.command({ "home" });
            journey.openLibrary("Journey Library");
            const int responsesBeforeReopen = fixture.episodeDetailResponses;
            journey.click("Journey Series");
            journey.await([&] { return fixture.episodeDetailResponses > responsesBeforeReopen; },
                "reopened episode details did not fetch stale provider UserData");
            const int episodesBeforeResume = fixture.episodeRequests;
            const qsizetype reportsBeforeResume = fixture.reports.size();
            journey.click("Resume", false, false, TextSurface::DetailPrimaryCaption);
            journey.await(
                [&] {
                    return fixture.episodeRequests > episodesBeforeResume
                        && journey.state()["playback"].toObject()["loaded"].toBool();
                },
                "Resume did not hydrate the selected episode queue and load real media");
            journey.pauseForObservation("resumed episode did not pause before observing its saved position");
            journey.await(
                [&] {
                    return std::any_of(fixture.reports.cbegin() + reportsBeforeResume, fixture.reports.cend(),
                        [expectedTicks](const QJsonObject& report) {
                            return report["endpoint"] == "/Sessions/Playing" && report["ItemId"] == "journey-episode"
                                && report["PositionTicks"].toVariant().toLongLong() == expectedTicks;
                        });
                },
                "selected episode Resume lost the stopped position during stale series hydration");
            const double expectedSeconds = double(expectedTicks) / 10000000.0;
            double resumePosition = -1;
            bool resumePaused = false;
            bool resumeGreen = false;
            try {
                journey.await(
                    [&] {
                        const auto playback = journey.state()["playback"].toObject();
                        resumePosition = playback["position"].toDouble();
                        resumePaused = playback["paused"].toBool();
                        // Allow generous pause-settlement headroom on slow simulator
                        // runners while still proving Resume started near expectedTicks.
                        if (resumePosition < expectedSeconds - 0.5 || resumePosition >= expectedSeconds + 15.0)
                            return false;
                        resumeGreen = frameHasBands(journey.capture(true), 1);
                        return resumeGreen;
                    },
                    "episode Resume did not decode the saved media position");
            } catch (const std::exception&) {
                throw std::runtime_error(QStringLiteral("episode Resume did not decode the saved media position "
                                                        "(expected=%1, position=%2, paused=%3, green=%4)")
                        .arg(expectedSeconds)
                        .arg(resumePosition)
                        .arg(resumePaused)
                        .arg(resumeGreen)
                        .toStdString());
            }
        };
        resumeEpisode(firstStopTicks);
        journey.command({ "seek", "20" });
        journey.await(
            [&] {
                const double position = journey.state()["playback"].toObject()["position"].toDouble();
                return position > 19.5 && position < 20.5 && frameHasBands(journey.capture(true), 1);
            },
            "second episode stop position was not reached");
        const qsizetype reportsBeforeSecondStop = fixture.reports.size();
        qint64 secondStopTicks = -1;
        fixturePhase = QStringLiteral("episode-second-stop");
        journey.command({ "stop" });
        journey.await([&] { return !journey.state()["playback"].toObject()["active"].toBool(); },
            "second episode stop did not settle");
        journey.await(
            [&] {
                return std::any_of(fixture.reports.cbegin() + reportsBeforeSecondStop, fixture.reports.cend(),
                    [&secondStopTicks](const QJsonObject& report) {
                        const qint64 ticks = report["PositionTicks"].toVariant().toLongLong();
                        if (report["endpoint"] != "/Sessions/Playing/Stopped" || report["ItemId"] != "journey-episode"
                            || ticks <= 195000000 || ticks >= 205000000)
                            return false;
                        secondStopTicks = ticks;
                        return true;
                    });
            },
            "second episode stop did not report its independently observed position");
        resumeEpisode(secondStopTicks);
        journey.command({ "stop" });
        journey.await([&] { return !journey.state()["playback"].toObject()["active"].toBool(); },
            "resumed episode did not stop before explicit restart");
        const qsizetype reportsBeforeRemote = fixture.reports.size();
        fixturePhase = QStringLiteral("episode-remote");
        // Use 5.5s so stopping preserves resume progress (>= 5s threshold in
        // isMeaningfulResumePosition, keeping "Start from beginning" visible
        // on fast runners) while still leaving 6.5s of pause-settlement
        // headroom before the 12s red-to-lime band boundary.
        require(fixture.sendRemotePlay(55000000), "real remote event socket was not connected");
        journey.await([&] { return journey.state()["playback"].toObject()["loaded"].toBool(); },
            "real inbound remote Play did not load the selected episode");
        journey.pauseForObservation("inbound remote episode did not pause before observing its requested position");
        journey.await(
            [&] {
                return std::any_of(fixture.reports.cbegin() + reportsBeforeRemote, fixture.reports.cend(),
                    [](const QJsonObject& report) {
                        return report["endpoint"] == "/Sessions/Playing" && report["ItemId"] == "journey-episode"
                            && report["PositionTicks"].toVariant().toLongLong() == 55000000;
                    });
            },
            "cached local resume progress overrode the explicit inbound remote start position");
        double remotePosition = -1;
        bool remotePaused = false;
        bool remoteRed = false;
        bool remoteGreen = false;
        try {
            journey.await(
                [&] {
                    const auto playback = journey.state()["playback"].toObject();
                    remotePosition = playback["position"].toDouble();
                    remotePaused = playback["paused"].toBool();
                    const auto frame = journey.capture(true);
                    remoteRed = frameHasBands(frame, 0);
                    remoteGreen = frameHasBands(frame, 1);
                    // On slow software simulators playback can advance past the 12s
                    // red-to-lime boundary before pause settles; verify the decoded
                    // band matches the paused position while staying strictly below
                    // the ~20s cached resume position.
                    const bool expectedBand = remotePosition < 11.8
                        ? remoteRed
                        : (remotePosition > 12.2 ? remoteGreen : (remoteRed || remoteGreen));
                    return remotePosition > 5.0 && remotePosition < 18.5 && expectedBand;
                },
                "inbound remote start did not decode the explicitly requested position");
        } catch (const std::exception&) {
            throw std::runtime_error(
                QStringLiteral("inbound remote start did not decode the explicitly requested position "
                               "(position=%1, paused=%2, red=%3, green=%4)")
                    .arg(remotePosition)
                    .arg(remotePaused)
                    .arg(remoteRed)
                    .arg(remoteGreen)
                    .toStdString());
        }
        journey.command({ "stop" });
        journey.await(
            [&] {
                // A dropped read during handoff is not a stopped player.
                // Keep polling for an explicit inactive state within await's deadline.
                const auto playback = journey.state(true)["playback"].toObject();
                return playback["active"].isBool() && !playback["active"].toBool();
            },
            "remote episode did not stop before explicit restart");
        journey.label("Start from beginning");
        const qsizetype reportsBeforeRestart = fixture.reports.size();
        fixturePhase = QStringLiteral("episode-restart");
        journey.click("Start from beginning", true);
        journey.await([&] { return journey.state()["playback"].toObject()["loaded"].toBool(); },
            "explicit episode restart did not load");
        journey.pauseForObservation("explicit episode restart did not pause before observing the beginning");
        journey.await(
            [&] {
                return std::any_of(fixture.reports.cbegin() + reportsBeforeRestart, fixture.reports.cend(),
                    [](const QJsonObject& report) {
                        return report["endpoint"] == "/Sessions/Playing" && report["ItemId"] == "journey-episode"
                            && report["PositionTicks"].toVariant().toLongLong() == 0;
                    });
            },
            "local resume precedence overrode explicit Play from start");
        double restartPosition = -1;
        bool restartRed = false;
        try {
            journey.await(
                [&] {
                    restartPosition = journey.state()["playback"].toObject()["position"].toDouble();
                    restartRed = frameHasBands(journey.capture(true), 0);
                    // The fixture stays red until 12s. Allow the same 0.2s
                    // timestamp/frame tolerance used at the remote-play boundary.
                    return restartPosition < 12.2 && restartRed;
                },
                "explicit episode restart did not decode the beginning");
        } catch (const std::exception&) {
            throw std::runtime_error(
                QStringLiteral("explicit episode restart did not decode the beginning (position=%1, red=%2)")
                    .arg(restartPosition)
                    .arg(restartRed)
                    .toStdString());
        }

        // EOF is driven by the real finite media, not a synthetic controller
        // signal. The decoded restart and server receipt prove repeat crossed
        // QML -> application queue -> mpv -> production provider reporting.
        fixturePhase = QStringLiteral("repeat-current-eof");
        journey.openPlaybackSettings();
        journey.click("Repeat", false, false, TextSurface::PlayerOptions);
        if (qEnvironmentVariable("SPOOL_E2E_ANDROID_TV") == "1") {
            journey.label("Repeat current item", false, TextSurface::PlayerOptions);
            journey.selectAndroidTvRepeatCurrent();
        } else {
            journey.click("Repeat current item", false, false, TextSurface::PlayerOptions);
        }
        const qsizetype reportsBeforeRepeat = fixture.reports.size();
        const int negotiationsBeforeRepeat = fixture.playbackNegotiations;
        journey.command({ "seek", "88" });
        journey.await([&] { return journey.state()["playback"].toObject()["position"].toDouble() > 87.5; },
            "repeat journey did not reach the end of the paused item");
        journey.command({ "resume" });
        QJsonObject repeatPlayback;
        bool repeatStartedAtZero = false;
        try {
            journey.await(
                [&] {
                    repeatPlayback = journey.state()["playback"].toObject();
                    repeatStartedAtZero = std::any_of(fixture.reports.cbegin() + reportsBeforeRepeat,
                        fixture.reports.cend(), [](const QJsonObject& report) {
                            return report["endpoint"] == "/Sessions/Playing" && report["ItemId"] == "journey-episode"
                                && report["RepeatMode"] == "RepeatOne"
                                && report["PositionTicks"].toVariant().toLongLong() == 0;
                        });
                    return fixture.playbackNegotiations > negotiationsBeforeRepeat && repeatPlayback["loaded"].toBool()
                        && repeatStartedAtZero;
                },
                "Repeat current item did not renegotiate and report an actual restart from zero");
        } catch (const std::exception& error) {
            throw std::runtime_error(
                QStringLiteral("%1 (negotiations=%2, reportedRestart=%3, active=%4, loaded=%5, paused=%6, position=%7)")
                    .arg(QString::fromUtf8(error.what()))
                    .arg(fixture.playbackNegotiations - negotiationsBeforeRepeat)
                    .arg(repeatStartedAtZero)
                    .arg(repeatPlayback["active"].toBool())
                    .arg(repeatPlayback["loaded"].toBool())
                    .arg(repeatPlayback["paused"].toBool())
                    .arg(repeatPlayback["position"].toDouble(), 0, 'f', 2)
                    .toStdString());
        }
        journey.pauseForObservation("repeated item did not pause after its first renderer restart");
        journey.await(
            [&] {
                const auto playback = journey.state()["playback"].toObject();
                return playback["loaded"].toBool() && playback["paused"].toBool()
                    && playback["position"].toDouble() < 11.5 && frameHasBands(journey.capture(true), 0);
            },
            "repeated item did not decode its beginning");

        // After-current has precedence over repeat-one. Leave repeat enabled:
        // merely clearing the repeat setting would make this a false positive.
        fixturePhase = QStringLiteral("sleep-after-current-eof");
        journey.openPlaybackSettings();
        journey.label("Current item", false, TextSurface::PlayerRepeatDetail);
        journey.click("Sleep timer", false, false, TextSurface::PlayerOptions);
        journey.click("15 minutes", false, false, TextSurface::PlayerOptions);
        journey.openPlaybackSettings();
        journey.label("remaining", false, TextSurface::PlayerSleepDetail);
        journey.click("Sleep timer", false, false, TextSurface::PlayerOptions);
        journey.click("After current item", false, false, TextSurface::PlayerOptions);
        journey.openPlaybackSettings();
        journey.label("After current item", false, TextSurface::PlayerSleepDetail);
        journey.label("Current item", false, TextSurface::PlayerRepeatDetail);
        journey.command({ "key", "back" });
        journey.command({ "seek", "88" });
        journey.await([&] { return journey.state()["playback"].toObject()["position"].toDouble() > 87.5; },
            "sleep journey did not reach the end of the paused item");
        const qsizetype reportsBeforeSleep = fixture.reports.size();
        const int negotiationsBeforeSleep = fixture.playbackNegotiations;
        journey.command({ "resume" });
        journey.await(
            [&] {
                return !journey.state()["playback"].toObject()["active"].toBool()
                    && std::any_of(fixture.reports.cbegin() + reportsBeforeSleep, fixture.reports.cend(),
                        [](const QJsonObject& report) {
                            return report["endpoint"] == "/Sessions/Playing/Stopped"
                                && report["ItemId"] == "journey-episode"
                                && report["PositionTicks"].toVariant().toLongLong() >= 880000000;
                        });
            },
            "After current item did not stop real playback and report the completed item's position");
        QElapsedTimer stoppedWindow;
        stoppedWindow.start();
        do {
            require(!journey.state()["playback"].toObject()["active"].toBool(),
                "sleep completion briefly stopped but repeat restarted playback");
            require(fixture.playbackNegotiations == negotiationsBeforeSleep,
                "sleep completion negotiated another item despite After current item");
            turn();
        } while (stoppedWindow.elapsed() < 1500);
        require(fixture.playedItems.contains("journey-episode"),
            "completed episode never persisted its watched state to the real provider");
        require(fixture.unexpected.isEmpty(), "fixture observed unexpected or unauthorized production traffic");
        fixturePhase = QStringLiteral("complete");
        std::cout << "app-journey: real login/filter/retained-details/settings/download/offline/online/repeat/sleep "
                     "GPU journey completed\n";
        return 0;
    } catch (const std::exception& error) {
        if (root.isValid()) {
            root.setAutoRemove(false);
            std::cerr << "app-journey: private failure artifacts retained at " << root.path().toStdString() << '\n';
        }
        std::cerr << "app-journey: failure phase " << fixturePhase.toStdString() << '\n';
        std::cerr << "app-journey: " << error.what() << '\n';
        return 1;
    }
}
