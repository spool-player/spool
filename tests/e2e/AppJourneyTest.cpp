#include "JellyfinFixture.h"
#include "TestMain.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRect>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
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
        : kind(std::move(kind)), environment(std::move(environment)), hostDescriptor(directory + "/device.json")
    {
    }
    void start(const QString& origin, const QString& instance)
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
            // The developer build of the actual product uses Qt's supported
            // argument bridge; no test APK stands in for the product.
            const QString arguments = "'--automation-port=0 --instance=" + instance + " --provider-store=" + origin + "/'";
            runTool({ "shell", "am", "start", "-W", "-n", bundle + "/com.sachk.spool.SpoolActivity",
                "--es", "applicationArguments", arguments });
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
            runTool({ "launch", "--terminate-running-process", device, bundle,
                "--automation-port=0", "--instance=" + instance, "--provider-store=" + origin + '/' });
            remoteScreenshot = container + "/Library/Caches/journey-frame.png";
        }
        QElapsedTimer deadline;
        deadline.start();
        QJsonObject descriptor;
        while (deadline.elapsed() < 60000) {
            QByteArray bytes;
            if (this->kind == "android") {
                try {
                    bytes = runTool({ "exec-out", "run-as", bundle, "cat", "cache/spool-control/" + instance + ".json" });
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
        if (this->kind == "android") {
            const QString forwarded = QString::fromUtf8(runTool({ "forward", "tcp:0",
                "tcp:" + QString::number(descriptor["port"].toInt()) })).trimmed();
            forwardPort = forwarded.toInt();
            require(forwardPort > 0 && forwardPort <= 65535, "private Android control port could not be forwarded");
            descriptor.insert("port", forwardPort);
        }
        QSaveFile file(hostDescriptor);
        const QByteArray bytes = QJsonDocument(descriptor).toJson(QJsonDocument::Compact);
        require(file.open(QIODevice::WriteOnly) && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                && file.write(bytes) == bytes.size() && file.commit(), "pulled descriptor could not be stored privately");
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
    QString descriptorPath() const { return hostDescriptor; }
    QString screenshotPath() const { return remoteScreenshot; }
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
    int forwardPort = 0;
    int reversePort = 0;
};
struct Words {
    QString text;
    QRect bounds;
};
class Journey final {
public:
    Journey(QString control, QProcessEnvironment environment, const QString& directory)
        : control(std::move(control)), environment(std::move(environment)), screenshotPath(directory + "/frame.png")
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
            app.setStandardOutputFile(QProcess::nullDevice());
            app.setStandardErrorFile(QProcess::nullDevice());
            app.start(executable, { "--instance", instance, "--data-dir", environment.value("SPOOL_DATA_HOME"),
                "--provider-store", origin + '/' });
            require(app.waitForStarted(10000), "built Spool could not start");
        } else {
            device = std::make_unique<DeviceSession>(deviceKind, QFileInfo(screenshotPath).absolutePath(), environment);
            device->start(origin, instance);
        }
        await([&] {
            const auto reply = command({ "status" }, {}, false);
            return reply["initialized"].toBool() && reply["window"].toObject()["exposed"].toBool();
        }, "Spool did not initialize an exposed GPU window", 60000);
    }
    QJsonObject command(const QStringList& args, const QByteArray& input = {}, bool required = true,
        const QString& descriptorOverride = {}, QJsonObject *envelope = nullptr)
    {
        require(device || app.state() != QProcess::NotRunning, "built Spool exited during the journey");
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setStandardErrorFile(QProcess::nullDevice());
        const QStringList destination = !descriptorOverride.isEmpty() ? QStringList { "--descriptor", descriptorOverride }
            : device ? QStringList { "--descriptor", device->descriptorPath() } : QStringList { "--instance", instance };
        process.start(control, destination + QStringList { "--timeout", "5000" } + args);
        require(process.waitForStarted(5000), "built spoolet could not start");
        if (!input.isEmpty())
            process.write(input);
        process.closeWriteChannel();
        require(finish(process, 10000), "spoolet exceeded its deadline");
        const auto response = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        if (envelope)
            *envelope = response;
        if (required)
            require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0
                    && response["ok"].toBool(), "production spoolet rejected a journey command");
        return response["ok"].toBool() ? response["result"].toObject() : QJsonObject {};
    }
    void assertControlBoundaries()
    {
        const QString genuine = device ? device->descriptorPath()
            : environment.value("SPOOL_DATA_HOME") + "/runtime/spool-control/" + instance + ".json";
        QFile input(genuine);
        require(input.open(QIODevice::ReadOnly), "actual app descriptor could not be read for the security control");
        QJsonObject forged = QJsonDocument::fromJson(input.read(4097)).object();
        require(forged["token"].toString().size() == 64, "actual app descriptor has no capability");
        forged.insert("token", QString(64, QLatin1Char('0')));
        const QString deniedPath = QFileInfo(genuine).dir().filePath("denied-capability.json");
        QSaveFile output(deniedPath);
        const QByteArray bytes = QJsonDocument(forged).toJson(QJsonDocument::Compact);
        require(output.open(QIODevice::WriteOnly) && output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                && output.write(bytes) == bytes.size() && output.commit(), "negative capability could not be stored privately");
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
    QJsonObject state() { return command({ "state" }); }
    QImage capture(bool video = false)
    {
        const auto result = command({ "screenshot", device ? device->screenshotPath() : screenshotPath });
        require(result["frameAwaited"].toBool(), "capture did not await a rendered frame");
        if (video)
            require(result["videoIncluded"].toBool(), "capture excludes native video; cannot claim GPU frame proof");
        if (device)
            device->pullScreenshot(screenshotPath);
        QImage image(screenshotPath);
        require(!image.isNull() && image.width() >= 640 && image.height() >= 360, "rendered screenshot is missing");
        return image;
    }
    QList<Words> words()
    {
        capture();
        const QByteArray tsv = run(ocr, { screenshotPath, "stdout", "-l", "eng", "--psm", "11", "tsv" }, environment);
        QList<Words> lines;
        QString previous;
        for (const QByteArray& row : tsv.split('\n')) {
            const auto fields = row.split('\t');
            if (fields.size() < 12 || fields[0] != "5" || fields[11].trimmed().isEmpty())
                continue;
            const QString key = QString::fromLatin1(fields[1] + ':' + fields[2] + ':' + fields[3] + ':' + fields[4]);
            const QRect bounds(fields[6].toInt(), fields[7].toInt(), fields[8].toInt(), fields[9].toInt());
            if (key != previous) {
                lines.append({ QString::fromUtf8(fields[11]).trimmed(), bounds });
                previous = key;
            } else {
                lines.last().text += ' ' + QString::fromUtf8(fields[11]).trimmed();
                lines.last().bounds = lines.last().bounds.united(bounds);
            }
        }
        return lines;
    }
    QRect label(const QString& text, bool scroll = false)
    {
        QRect found;
        const std::string failure = "expected user-visible label was not rendered: " + text.toStdString();
        await([&] {
            const auto rendered = words();
            for (const auto& line : rendered) {
                if (line.text.compare(text, Qt::CaseInsensitive) == 0) {
                    found = line.bounds;
                    return true;
                }
            }
            for (const auto& line : rendered) {
                if (line.text.contains(text, Qt::CaseInsensitive)) {
                    found = line.bounds;
                    return true;
                }
            }
            if (scroll)
                command({ "key", "down" });
            return false;
        }, failure.c_str(), scroll ? 90000 : 30000);
        return found;
    }
    void click(const QString& text, bool scroll = false, bool right = false)
    {
        const QRect bounds = label(text, scroll);
        const auto window = state()["window"].toObject();
        const double ratio = window["devicePixelRatio"].toDouble(1);
        command({ "pointer", right ? "right-click" : "click", QString::number(bounds.center().x() / ratio),
            QString::number(bounds.center().y() / ratio) });
        turn(200);
    }
    void await(const std::function<bool()>& predicate, const char *message, int timeout = 30000)
    {
        QElapsedTimer clock;
        clock.start();
        do {
            require(device || app.state() != QProcess::NotRunning, "built Spool exited during the journey");
            if (predicate())
                return;
            turn();
        } while (clock.elapsed() < timeout);
        throw std::runtime_error(message);
    }
    QProcess app;
private:
    QString control;
    QProcessEnvironment environment;
    QString screenshotPath;
    QString ocr;
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
    run(ffmpeg, { "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
        "color=c=blue:s=640x360:r=10:d=30", "-vf",
        "drawbox=x=0:y=0:w=640:h=180:color=red:t=fill:enable='lt(t,10)',"
        "drawbox=x=0:y=0:w=640:h=180:color=lime:t=fill:enable='gte(t,10)'",
        "-an", "-c:v", "ffv1", "-level", "3", "-g", "1", "-threads", "1", path }, environment, {}, 60000);
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
        if (key.startsWith("SPOOL_") && key != "SPOOL_E2E_ISOLATED_DISPLAY"
            && !key.startsWith("SPOOL_E2E_") && key != "SPOOL_GL_BACKEND")
            environment.remove(key);
    }
    for (const auto& pair : { std::pair { "SPOOL_DATA_HOME", "app-data" },
             { "SPOOL_CREDENTIAL_STORE_DIR", "credentials" } }) {
        const QString path = directory + '/' + pair.second;
        require(QDir().mkpath(path), "isolated app directory could not be created");
        environment.insert(pair.first, path);
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
        QTemporaryDir root;
        require(root.isValid(), "journey temporary directory could not be created");
        const auto environment = isolatedEnvironment(root.path());
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
        const bool television = deviceKind == "tvos"
            || (deviceKind == "android" && qEnvironmentVariable("SPOOL_E2E_ANDROID_TV") == "1");
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
        journey.await([&] { return fixture.rejectedLogins == 1; }, "wrong password was not rejected by the real login path");
        journey.label("Wrong username or password");
        journey.text("journey-password");
        journey.command({ "key", "ok" });
        journey.await([&] { return fixture.successfulLogins == 1 && fixture.authenticatedViews > 0; },
            "login did not activate an authenticated provider library");
        journey.click("Journey Library");
        journey.await([&] { return fixture.authenticatedBrowse > 0 && !journey.state()["browseLoading"].toBool(); },
            "browse did not request the selected library");
        journey.label("Journey Film");
        journey.await([&] { return posterVisible(journey.capture()); },
            "browse title is present but independent fixture artwork is not rendered");
        const auto rows = journey.command({ "items", "browse" })["items"].toArray();
        require(rows.size() == 1 && rows[0].toObject()["title"] == "Journey Film"
                && rows[0].toObject()["playable"].toBool(), "visible browse did not expose the expected playable item");
        const QString itemId = rows[0].toObject()["id"].toString();
        require(itemId.endsWith(":journey-movie") && itemId != "journey-movie", "provider item lost account scoping");

        // Open and operate the real item-menu/download dialog; list is only an observer.
        journey.click("Journey Film", false, true);
        journey.click("Download");
        journey.click("Original");
        QString jobId;
        journey.await([&] {
            const auto jobs = journey.command({ "downloads", "list" })["jobs"].toArray();
            if (jobs.size() != 1)
                return false;
            const auto job = jobs[0].toObject();
            require(!job["failed"].toBool(), "real provider download failed");
            jobId = job["id"].toString();
            return job["state"] == "complete" && job["received"].toInteger() == fixture.mediaSize();
        }, "download did not finish with the exact independent server byte count", 60000);
        require(!jobId.isEmpty() && fixture.mediaBytes >= fixture.mediaSize(), "download never fetched the fixture media");
        journey.label("Downloaded");
        journey.command({ "back" });

        journey.command({ "navigate", "settings" });
        journey.label("Settings");
        const bool originalPreview = journey.command({ "settings", "get", "playback/seekPreviews" })["values"]
            .toObject()["playback/seekPreviews"].toBool();
        journey.click("Seek previews", true);
        journey.await([&] {
            return journey.command({ "settings", "get", "playback/seekPreviews" })["values"]
                .toObject()["playback/seekPreviews"].toBool() != originalPreview;
        }, "settings UI toggle did not commit its public value");
        journey.click("Play saved files offline", true);
        journey.label("Journey Film");
        journey.label("Downloaded");
        // Removing remote availability proves this is saved-media playback, not
        // another network resolve disguised as a completed-download echo.
        fixture.setMediaAvailable(false);
        const int remoteRequests = fixture.mediaRequests;
        journey.click("Play");
        journey.await([&] {
            const auto playback = journey.state()["playback"].toObject();
            require(!playback["failed"].toBool(), "saved-media playback failed");
            return playback["loaded"].toBool() && playback["embeddedVideo"].toBool()
                && playback["duration"].toDouble() > 29;
        }, "saved download did not enter real embedded playback");
        journey.command({ "pause" });
        journey.command({ "seek", "2" });
        journey.await([&] { return frameHasBands(journey.capture(true), 0); }, "GPU video did not render upright red-over-blue bands");
        journey.command({ "seek", "14" });
        journey.await([&] {
            return journey.state()["playback"].toObject()["position"].toDouble() > 12
                && frameHasBands(journey.capture(true), 1);
        }, "seek state advanced but the GPU frame did not change to green-over-blue");
        journey.command({ "resume" });
        const double before = journey.state()["playback"].toObject()["position"].toDouble();
        journey.await([&] { return journey.state()["playback"].toObject()["position"].toDouble() > before + 0.5; },
            "real media clock did not progress after resume");
        require(fixture.mediaRequests == remoteRequests, "saved-media playback unexpectedly fetched the unavailable server");
        journey.command({ "stop" });
        journey.await([&] { return !journey.state()["playback"].toObject()["active"].toBool(); }, "player did not stop");

        fixture.setMediaAvailable(true);
        const qsizetype reportsBeforeOnline = fixture.reports.size();
        journey.command({ "home" });
        journey.click("Journey Library");
        journey.click("Journey Film");
        journey.click("Play");
        journey.await([&] { return fixture.playbackNegotiations > 0 && journey.state()["playback"].toObject()["loaded"].toBool(); },
            "online UI play did not negotiate and load real provider media");
        journey.await([&] {
            return std::any_of(fixture.reports.cbegin() + reportsBeforeOnline, fixture.reports.cend(),
                [](const QJsonObject& report) {
                    return report["endpoint"] == "/Sessions/Playing" && report["ItemId"] == "journey-movie";
                });
        }, "real provider playback start for the online item was not reported");
        journey.command({ "seek", "14" });
        journey.await([&] { return frameHasBands(journey.capture(true), 1); }, "online GPU playback did not deliver negotiated media");
        journey.command({ "stop" });
        journey.await([&] {
            return std::any_of(fixture.reports.cbegin() + reportsBeforeOnline, fixture.reports.cend(), [](const QJsonObject& report) {
                return report["endpoint"] == "/Sessions/Playing/Stopped" && report["PositionTicks"].toVariant().toLongLong() > 100000000;
            });
        }, "provider stop receipt did not independently observe the played media position");
        require(fixture.unexpected.isEmpty(), "fixture observed unexpected or unauthorized production traffic");
        std::cout << "app-journey: real login/browse/settings/download/offline/online GPU journey completed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "app-journey: " << error.what() << '\n';
        return 1;
    }
}
