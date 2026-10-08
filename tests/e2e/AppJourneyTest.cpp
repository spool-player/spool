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
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLibraryInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRect>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
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
QString executable(const char *variable, const QString& fallback)
{
    const QString requested = qEnvironmentVariable(variable);
    const QString result = requested.isEmpty() ? fallback : requested;
    require(!result.isEmpty() && QFileInfo(result).isExecutable(), "required built app or runtime tool is unavailable");
    return result;
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
            reversePort = QUrl(origin).port();
            runTool({ "shell", "am", "force-stop", bundle });
            require(runTool({ "shell", "pm", "clear", bundle }).contains("Success"),
                "private emulator application sandbox could not be reset");
            runTool({ "reverse", "tcp:" + QString::number(reversePort), "tcp:" + QString::number(reversePort) });
            remoteScreenshot = "/data/user/0/" + bundle + "/cache/journey-frame.png";
        } else {
            require(this->kind == "tvos", "unknown mobile journey device");
            device = qEnvironmentVariable("SPOOL_TVOS_DEVICE");
            require(!device.isEmpty(), "tvOS journey requires an explicit private simulator UUID");
            tool = executable("SPOOL_E2E_XCRUN", QStandardPaths::findExecutable("xcrun"));
            bundle = qEnvironmentVariable("SPOOL_E2E_TVOS_BUNDLE", "com.sachk.spool");
            container = QString::fromUtf8(runTool({ "get_app_container", device, bundle, "data" })).trimmed();
            require(QDir::isAbsolutePath(container) && QDir(container).exists(),
                "installed actual tvOS app data container is unavailable");
            remoteScreenshot = container + "/Library/Caches/journey-frame.png";
        }
        assertDefaultAutomationOff(origin, instance + "-default", ocr);
        launchProduct({ "--automation-port=0", "--instance=" + instance, "--provider-store=" + origin + '/' });
        QElapsedTimer deadline;
        deadline.start();
        QJsonObject descriptor;
        while (deadline.elapsed() < 60000) {
            QByteArray bytes;
            if (this->kind == "android") {
                try {
                    bytes
                        = runTool({ "exec-out", "run-as", bundle, "cat", "cache/spool-control/" + instance + ".json" });
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
            const QString forwarded = QString::fromUtf8(
                runTool({ "forward", "tcp:0", "tcp:" + QString::number(descriptor["port"].toInt()) }))
                                          .trimmed();
            forwardPort = forwarded.toInt();
            require(forwardPort > 0 && forwardPort <= 65535, "private Android control port could not be forwarded");
            descriptor.insert("port", forwardPort);
        }
        QSaveFile file(hostDescriptor);
        const QByteArray bytes = QJsonDocument(descriptor).toJson(QJsonDocument::Compact);
        require(file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                && file.write(bytes) == bytes.size() && file.commit(),
            "pulled descriptor could not be stored privately");
    }
    ~DeviceSession()
    {
        try {
            if (kind == "android") {
                runTool({ "shell", "am", "force-stop", bundle });
                if (forwardPort > 0)
                    runTool({ "forward", "--remove", "tcp:" + QString::number(forwardPort) });
                if (reversePort > 0)
                    runTool({ "reverse", "--remove", "tcp:" + QString::number(reversePort) });
            } else {
                runTool({ "terminate", device, bundle });
            }
        } catch (const std::exception&) {
            // The lifecycle driver still owns and destroys the private emulator.
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
                const QJsonObject evidence { { "platform", kind }, { "pid", processPid }, { "signal", signal } };
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
    QString screenshotPath() const
    {
        return remoteScreenshot;
    }
    void deleteScreenshot()
    {
        if (kind == "android") {
            runTool({ "exec-out", "run-as", bundle, "rm", "-f", "cache/journey-frame.png" });
        } else {
            require(!QFileInfo::exists(remoteScreenshot) || QFile::remove(remoteScreenshot),
                "previous actual simulator screenshot could not be removed");
        }
    }
    void pullScreenshot(const QString& destination)
    {
        QByteArray bytes;
        if (kind == "android")
            bytes = runTool({ "exec-out", "run-as", bundle, "cat", "cache/journey-frame.png" });
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
        processPid = 0;
        if (kind == "android") {
            bool valid = false;
            deviceLaunchEpoch = QString::fromUtf8(runTool({ "shell", "date", "+%s" })).trimmed().toLongLong(&valid);
            require(valid && deviceLaunchEpoch > 0, "private emulator launch clock could not be observed");
            // Qt's developer-product argument bridge is the actual app entry,
            // not a test APK or an automation-only activity.
            runTool({ "shell", "am", "start", "-W", "-n", bundle + "/com.sachk.spool.SpoolActivity", "--es",
                "applicationArguments", "'" + arguments.join(' ') + "'" });
            processPid = QString::fromUtf8(runTool({ "shell", "pidof", "-s", bundle })).trimmed().toLongLong();
        } else {
            const QString output = QString::fromUtf8(
                runTool(QStringList { "launch", "--terminate-running-process", device, bundle } + arguments));
            const auto match = QRegularExpression(QStringLiteral(":\\s*([0-9]+)\\s*$")).match(output);
            if (match.hasMatch())
                processPid = match.captured(1).toLongLong();
        }
        require(processPid > 0, "actual mobile product launch did not expose its OS process identity");
    }
    void stopProduct()
    {
        if (kind == "android")
            runTool({ "shell", "am", "force-stop", bundle });
        else
            runTool({ "terminate", device, bundle });
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
    void assertDefaultAutomationOff(const QString& origin, const QString& instance, const QString& ocr)
    {
        launchProduct({ "--instance=" + instance, "--provider-store=" + origin + '/' });
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
            const QByteArray text = run(ocr, { screenshot, "stdout", "-l", "eng", "--psm", "11" }, environment);
            if (QString::fromUtf8(text).contains("Add a provider", Qt::CaseInsensitive)) {
                rendered = true;
                break;
            }
            turn(200);
        }
        require(rendered && productRunning(), "actual mobile normal launch did not render its product UI");
        if (kind == "android") {
            const QByteArray listing = runTool({ "exec-out", "run-as", bundle, "sh", "-c",
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
                    = QJsonDocument::fromJson(runTool({ "exec-out", "run-as", bundle, "cat", name })).object();
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
    int forwardPort = 0;
    int reversePort = 0;
};
enum class TextSurface { Window, LibraryRow };
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
        const QString& descriptorOverride = {}, QJsonObject *envelope = nullptr)
    {
        ensureRunning();
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setStandardErrorFile(QProcess::nullDevice());
        const QStringList destination = !descriptorOverride.isEmpty()
            ? QStringList { "--descriptor", descriptorOverride }
            : device ? QStringList { "--descriptor", device->descriptorPath() }
                     : QStringList { "--instance", instance };
        process.start(control, destination + QStringList { "--timeout", "5000" } + args);
        require(process.waitForStarted(5000), "built spoolet could not start");
        if (!input.isEmpty())
            process.write(input);
        process.closeWriteChannel();
        require(finish(process, 10000), "spoolet exceeded its deadline");
        ensureRunning();
        const auto response = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        const QString transportCode = response["error"].toObject()["code"].toString();
        if (device
            && (transportCode == "unavailable" || transportCode == "timeout" || transportCode == "invalid_response"))
            device->propagateObservedCrash();
        if (envelope)
            *envelope = response;
        if (required
            && (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || !response["ok"].toBool())) {
            const QString code = response["error"].toObject()["code"].toString();
            const bool safeCode = code.size() <= 64 && std::all_of(code.cbegin(), code.cend(), [](QChar character) {
                return character == '_' || (character >= 'a' && character <= 'z');
            });
            QString geometry;
            if (args.value(0) == "screenshot" && code == "not_exposed") {
                const auto window = command({ "status" }, {}, false)["window"].toObject();
                geometry = QString(" exposed=%1 width=%2 height=%3")
                               .arg(window["exposed"].toBool())
                               .arg(window["width"].toInt())
                               .arg(window["height"].toInt());
            }
            throw std::runtime_error(QString("production spoolet rejected %1 (exit=%2, code=%3)%4")
                    .arg(args.value(0))
                    .arg(process.exitCode())
                    .arg(safeCode ? code : QStringLiteral("invalid_response"))
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
    void text(const QString& value)
    {
        require(command({ "text", "--stdin" }, value.toUtf8())["accepted"].toBool(),
            "focused editable field did not accept production IME input");
    }
    QJsonObject state()
    {
        return command({ "state" });
    }
    QImage capture(bool video = false)
    {
        QJsonObject result;
        await(
            [&] {
                const auto window = state()["window"].toObject();
                if (!window["exposed"].toBool() || window["width"].toInt() <= 0 || window["height"].toInt() <= 0)
                    return false;
                for (const QString& previous : { screenshotPath, libraryTextPath() })
                    require(!QFileInfo::exists(previous) || QFile::remove(previous),
                        "previous host screenshot could not be removed");
                if (device)
                    device->deleteScreenshot();
                QJsonObject response;
                result = command(
                    { "screenshot", device ? device->screenshotPath() : screenshotPath }, {}, false, {}, &response);
                if (!response["ok"].toBool()) {
                    // Exposure can change between separate IPC requests while
                    // the real platform replaces a startup/fullscreen surface.
                    require(response["error"].toObject()["code"] == "not_exposed",
                        "actual framebuffer acquisition failed unexpectedly");
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
        require(!image.isNull() && image.width() >= 640 && image.height() >= 360, "rendered screenshot is missing");
        return image;
    }
    QList<Words> words(const QString& imagePath, int mode, const QPoint& origin = {})
    {
        const QByteArray tsv
            = run(ocr, { imagePath, "stdout", "-l", "eng", "--psm", QString::number(mode), "tsv" }, environment);
        QList<Words> lines;
        QString previous;
        for (const QByteArray& row : tsv.split('\n')) {
            const auto fields = row.split('\t');
            if (fields.size() < 12 || fields[0] != "5" || fields[11].trimmed().isEmpty())
                continue;
            const QString key = QString::fromLatin1(fields[1] + ':' + fields[2] + ':' + fields[3] + ':' + fields[4]);
            const QRect bounds(
                fields[6].toInt() + origin.x(), fields[7].toInt() + origin.y(), fields[8].toInt(), fields[9].toInt());
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
        const std::string failure = "expected user-visible label was not rendered: " + text.toStdString();
        const auto locate = [&](const QList<Words>& lines) {
            for (const auto& line : lines)
                if (line.text.compare(text, Qt::CaseInsensitive) == 0)
                    return line.phraseBounds(text);
            for (const auto& line : lines) {
                const QRect bounds = line.phraseBounds(text);
                if (!bounds.isEmpty())
                    return bounds;
            }
            return QRect {};
        };
        await(
            [&] {
                const QImage frame = capture();
                QString imagePath = screenshotPath;
                QRect region;
                if (surface == TextSurface::LibraryRow) {
                    const auto row = capturedState["visual"].toObject()["libraryRow"].toObject();
                    const double ratio = capturedState["window"].toObject()["devicePixelRatio"].toDouble(1);
                    region = QRectF(row["x"].toDouble() * ratio, row["y"].toDouble() * ratio,
                        row["width"].toDouble() * ratio, row["height"].toDouble() * ratio)
                                 .toAlignedRect()
                                 .intersected(frame.rect());
                    if (region.isEmpty())
                        return false;
                    imagePath = libraryTextPath();
                    QSaveFile file(imagePath);
                    require(file.open(QIODevice::WriteOnly)
                            && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                            && frame.copy(region).save(&file, "PNG") && file.commit(),
                        "actual rendered library row could not be read for caption navigation");
                }
                QRect candidate
                    = locate(words(imagePath, surface == TextSurface::LibraryRow ? 6 : 11, region.topLeft()));
                // Sparse-page segmentation can omit text touching a focus outline.
                // Read the same real frame as a text block rather than accepting a
                // larger heading's unrelated hit region or manufacturing a label.
                if (candidate.isEmpty() && surface == TextSurface::Window)
                    candidate = locate(words(imagePath, 6));
                if (!candidate.isEmpty()) {
                    if (surface == TextSurface::Window
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
        return found;
    }
    void click(const QString& text, bool scroll = false, bool right = false)
    {
        tap(label(text, scroll), right);
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
    void await(const std::function<bool()>& predicate, const char *message, int timeout = 30000)
    {
        QElapsedTimer clock;
        clock.start();
        do {
            ensureRunning();
            if (predicate())
                return;
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
            "color=c=blue:s=640x360:r=10:d=30", "-vf",
            "drawbox=x=0:y=0:w=640:h=180:color=red:t=fill:enable='lt(t,10)',"
            "drawbox=x=0:y=0:w=640:h=180:color=lime:t=fill:enable='gte(t,10)'",
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
    try {
        QString deviceKind;
        const QStringList arguments = application.arguments().mid(1);
        for (qsizetype index = 0; index < arguments.size(); ++index) {
            if (arguments[index] == "--device") {
                require(index + 1 < arguments.size(), "--device requires android or tvos");
                deviceKind = arguments[++index];
                require(deviceKind == "android" || deviceKind == "tvos", "--device requires android or tvos");
            }
        }
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
        require(fixture.listening(), "isolated loopback fixture could not listen");
        Journey journey(control, environment, root.path());
        journey.launch(app, fixture.origin(), deviceKind);
        journey.label("Add a provider");
        journey.assertControlBoundaries();
        const bool television
            = deviceKind == "tvos" || (deviceKind == "android" && qEnvironmentVariable("SPOOL_E2E_ANDROID_TV") == "1");
        journey.click("Jellyfin", true);
        journey.label("Choose a server");
        if (television)
            journey.command({ "key", "ok" });
        journey.text(fixture.origin());
        journey.command({ "key", "ok" });
        journey.label("Username");
        if (television)
            journey.command({ "key", "ok" });
        journey.text("journey");
        journey.command({ "key", "ok" });
        if (television)
            journey.command({ "key", "ok" });
        journey.text("incorrect-password");
        journey.command({ "key", "ok" });
        journey.await(
            [&] { return fixture.rejectedLogins == 1; }, "wrong password was not rejected by the real login path");
        journey.label("Wrong username or password");
        journey.text("journey-password");
        journey.command({ "key", "ok" });
        journey.await([&] { return fixture.successfulLogins == 1 && fixture.authenticatedViews > 0; },
            "login did not activate an authenticated provider library");
        journey.openLibrary("Journey Library");
        journey.await([&] { return fixture.authenticatedBrowse > 0 && !journey.state()["browseLoading"].toBool(); },
            "browse did not request the selected library");
        journey.label("Journey Film");
        journey.await([&] { return posterVisible(journey.capture()); },
            "browse title is present but independent fixture artwork is not rendered");
        const auto rows = journey.command({ "items", "browse" })["items"].toArray();
        require(rows.size() == 1 && rows[0].toObject()["title"] == "Journey Film"
                && rows[0].toObject()["playable"].toBool(),
            "visible browse did not expose the expected playable item");
        const QString itemId = rows[0].toObject()["id"].toString();
        require(itemId.endsWith(":journey-movie") && itemId != "journey-movie", "provider item lost account scoping");

        // Open and operate the real item-menu/download dialog; list is only an observer.
        journey.click("Journey Film", false, true);
        journey.click("Download");
        // The actual chooser initially focuses Original. Confirm through the
        // same keyboard/D-pad activation a remote viewer uses, not an API start.
        journey.command({ "key", "ok" });
        QString jobId;
        journey.await(
            [&] {
                const auto jobs = journey.command({ "downloads", "list" })["jobs"].toArray();
                if (jobs.size() != 1)
                    return false;
                const auto job = jobs[0].toObject();
                require(!job["failed"].toBool(), "real provider download failed");
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
        journey.click("Play saved files offline", true);
        journey.label("Journey Film");
        journey.label("Downloaded");
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
        journey.await(
            [&] {
                const auto playback = journey.state()["playback"].toObject();
                require(!playback["failed"].toBool(), "saved-media playback failed");
                return playback["loaded"].toBool() && playback["embeddedVideo"].toBool()
                    && playback["duration"].toDouble() > 29;
            },
            "saved download did not enter real embedded playback");
        journey.command({ "pause" });
        journey.await([&] { return journey.state()["playback"].toObject()["paused"].toBool(); },
            "asynchronous pause did not settle before exact paused-frame seeking");
        journey.command({ "seek", "2" });
        journey.await(
            [&] {
                const auto frame = journey.capture(true);
                const double position = journey.state()["playback"].toObject()["position"].toDouble();
                return position > 1.5 && position < 2.5 && frameHasBands(frame, 0);
            },
            "exact seek to 2 seconds did not render upright red-over-blue bands");
        journey.command({ "seek", "14" });
        double seekPosition = 0;
        bool greenFrame = false;
        try {
            journey.await(
                [&] {
                    greenFrame = frameHasBands(journey.capture(true), 1);
                    seekPosition = journey.state()["playback"].toObject()["position"].toDouble();
                    return seekPosition > 12 && greenFrame;
                },
                "seek did not deliver the requested decoded frame");
        } catch (const std::exception&) {
            throw std::runtime_error(QStringLiteral("exact seek14 failed: public position=%1, green-over-blue=%2")
                    .arg(seekPosition)
                    .arg(greenFrame)
                    .toStdString());
        }
        journey.command({ "resume" });
        const double before = journey.state()["playback"].toObject()["position"].toDouble();
        journey.await([&] { return journey.state()["playback"].toObject()["position"].toDouble() > before + 0.5; },
            "real media clock did not progress after resume");
        journey.command({ "stop" });
        journey.await(
            [&] { return !journey.state()["playback"].toObject()["active"].toBool(); }, "player did not stop");
        require(fixture.authenticatedRequests == authenticatedBeforeOffline,
            "saved-media playback contacted an unavailable authenticated provider endpoint");
        require(fixture.playbackNegotiations == negotiationsBeforeOffline,
            "saved-media playback renegotiated against the unavailable provider");
        require(fixture.mediaRequests == remoteRequests,
            "saved-media playback unexpectedly fetched the unavailable server");

        fixture.setNewRequestsAvailable(true);
        const qsizetype reportsBeforeOnline = fixture.reports.size();
        journey.command({ "home" });
        journey.openLibrary("Journey Library");
        journey.click("Journey Film");
        journey.command({ "key", "ok" });
        journey.await(
            [&] {
                return fixture.playbackNegotiations > 0 && journey.state()["playback"].toObject()["loaded"].toBool();
            },
            "online UI play did not negotiate and load real provider media");
        journey.await(
            [&] {
                return std::any_of(fixture.reports.cbegin() + reportsBeforeOnline, fixture.reports.cend(),
                    [](const QJsonObject& report) {
                        return report["endpoint"] == "/Sessions/Playing" && report["ItemId"] == "journey-movie";
                    });
            },
            "real provider playback start for the online item was not reported");
        journey.command({ "seek", "14" });
        journey.await([&] { return frameHasBands(journey.capture(true), 1); },
            "online GPU playback did not deliver negotiated media");
        journey.command({ "stop" });
        journey.await(
            [&] {
                return std::any_of(fixture.reports.cbegin() + reportsBeforeOnline, fixture.reports.cend(),
                    [](const QJsonObject& report) {
                        return report["endpoint"] == "/Sessions/Playing/Stopped"
                            && report["PositionTicks"].toVariant().toLongLong() > 100000000;
                    });
            },
            "provider stop receipt did not independently observe the played media position");
        if (!fixture.unexpected.isEmpty()) {
            // Retain only rejection categories/paths, never headers or bodies.
            QSaveFile diagnostics(root.path() + "/fixture-rejections.json");
            const auto bytes = QJsonDocument(QJsonArray::fromStringList(fixture.unexpected)).toJson();
            require(diagnostics.open(QIODevice::WriteOnly)
                    && diagnostics.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                    && diagnostics.write(bytes) == bytes.size() && diagnostics.commit(),
                "private fixture rejection receipt could not be retained");
        }
        require(fixture.unexpected.isEmpty(), "fixture observed unexpected or unauthorized production traffic");
        std::cout << "app-journey: real login/browse/settings/download/offline/online GPU journey completed\n";
        return 0;
    } catch (const std::exception& error) {
        if (root.isValid()) {
            root.setAutoRemove(false);
            std::cerr << "app-journey: private failure artifacts retained at " << root.path().toStdString() << '\n';
        }
        std::cerr << "app-journey: " << error.what() << '\n';
        return 1;
    }
}
