#include "LocalControlProtocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextStream>
#include <cmath>
#include <cstdio>

namespace {
int print(const QJsonObject& result)
{
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n';
    return result.value(QStringLiteral("ok")).toBool() ? 0 : 1;
}
int error(const QString& message)
{
    return print(Spool::LocalControl::failure({}, QStringLiteral("cli_error"), message));
}
QJsonObject help()
{
    return { { QStringLiteral("ok"), true },
        { QStringLiteral("result"),
            QJsonObject { { QStringLiteral("usage"),
                              QStringLiteral("spoolet [--host USER@HOST] [--instance ID | --descriptor FILE] "
                                             "[--timeout MS] COMMAND [ARGS]") },
                { QStringLiteral("commands"),
                    QJsonArray { QStringLiteral("help | instances | status | state"),
                        QStringLiteral(
                            "navigate home|search|settings | home | back | key up|down|left|right|ok|back|space"),
                        QStringLiteral("navigate providerDetails MODULE_ID | navigate subtitleSettings [SETTING_KEY]"),
                        QStringLiteral("items [browse|libraries|resume|next-up] [OFFSET] [LIMIT] | library "
                                       "QUALIFIED_ID | load-more"),
                        QStringLiteral("browse sort SORT_KEY [Ascending|Descending] (server-side library ordering)"),
                        QStringLiteral("play QUALIFIED_ID [--from-start] | pause | resume | stop | seek SECONDS"),
                        QStringLiteral("preview SECONDS (actual timeline hover; key up first if controls are hidden)"),
                        QStringLiteral("qualities | quality INDEX"), QStringLiteral("screenshot FILE.png"),
                        QStringLiteral("pointer move|click|press|release|right-click X Y (window logical coordinates)"),
                        QStringLiteral("text TEXT | text --stdin (commit to the focused editable field; no echo)"),
                        QStringLiteral(
                            "downloads list | options ITEM_ID | start ITEM_ID INDEX | cancel|retry|remove|play JOB_ID"),
                        QStringLiteral("settings get [KEY] | settings set KEY JSON_VALUE"),
                        QStringLiteral("speed-test [status] (start measurement, or read numeric progress and results)"),
                        QStringLiteral("open browse|resume|next-up|context|similar INDEX (select like a person would)"),
                        QStringLiteral("item-state QUALIFIED_ID [played|favorite true|false | position SECONDS] "
                                       "(writes through the provider, then reads the item back from the server)"),
                        QStringLiteral("capture start ABS_DIR [SETTLE_MS] [MAX_MS] | capture status | capture stop "
                                       "(saves each visibly changed frame until the screen settles)") } },
                { QStringLiteral("notes"),
                    QStringLiteral(
                        "All stdout is JSON. Commands are bounded and never prompt. When multiple instances run, "
                        "--instance is required. Playback/navigation acceptance is not a promise that provider I/O has "
                        "completed: poll status. Screenshot waits for a new Qt frame, then atomically writes PNG; "
                        "separate native video planes are reported as excluded. --host uses noninteractive SSH "
                        "with an already trusted host key and forwards stdin to remote IPv4 loopback; no TV CLI "
                        "or LAN listener is needed. With --host, --descriptor is an absolute REMOTE private "
                        "descriptor path; otherwise it is local. --instance selects a discovered instance on "
                        "the selected host. Tokens are never printed.") } } } };
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("spoolet"));
    QStringList arguments = app.arguments().mid(1);
    QString instance;
    QString descriptorPath;
    QString host;
    int timeout = Spool::LocalControl::RequestTimeoutMs;
    while (!arguments.isEmpty() && arguments.first().startsWith(QStringLiteral("--"))) {
        const QString option = arguments.takeFirst();
        if (option == QStringLiteral("--help"))
            return print(help());
        if ((option != QStringLiteral("--instance") && option != QStringLiteral("--timeout")
                && option != QStringLiteral("--descriptor") && option != QStringLiteral("--host"))
            || arguments.isEmpty())
            return error(QStringLiteral("Expected --host, --instance, --descriptor or --timeout; see help"));
        const QString value = arguments.takeFirst();
        if (option == QStringLiteral("--instance")) {
            if (!Spool::LocalControl::validInstance(value))
                return error(QStringLiteral("Invalid instance identifier"));
            instance = value;
        } else if (option == QStringLiteral("--descriptor")) {
            descriptorPath = value;
        } else if (option == QStringLiteral("--host")) {
            if (!Spool::LocalControl::validSshHost(value))
                return error(QStringLiteral("Invalid SSH host; use [USER@]HOST"));
            host = value;
        } else {
            bool valid = false;
            timeout = value.toInt(&valid);
            if (!valid || timeout < 100 || timeout > 30'000)
                return error(QStringLiteral("Timeout must be 100..30000 milliseconds"));
        }
    }
    if (!instance.isEmpty() && !descriptorPath.isEmpty())
        return error(QStringLiteral("--instance and --descriptor are mutually exclusive"));
    if (!descriptorPath.isEmpty()) {
        if (!host.isEmpty() && !QDir::isAbsolutePath(descriptorPath))
            return error(QStringLiteral("Remote descriptor path must be absolute"));
        if (host.isEmpty())
            descriptorPath = QFileInfo(descriptorPath).absoluteFilePath();
    }
    if (arguments.isEmpty() || arguments == QStringList { QStringLiteral("help") })
        return print(help());
    QString command = arguments.takeFirst();
    QJsonObject args;
    const auto count = [&](int low, int high) { return arguments.size() >= low && arguments.size() <= high; };
    if (command == QStringLiteral("speed-test")) {
        if (!arguments.isEmpty() && arguments != QStringList { QStringLiteral("status") })
            return error(QStringLiteral("Usage: speed-test [status]"));
        args.insert(QStringLiteral("refresh"), arguments.isEmpty());
    } else if (command == QStringLiteral("navigate")) {
        if (!count(1, 2))
            return error(QStringLiteral("Usage: navigate ROUTE [MODULE_ID|SETTING_KEY]"));
        const QString route = arguments.first();
        args.insert(QStringLiteral("route"), route);
        if (route == QStringLiteral("providerDetails")) {
            if (!count(2, 2))
                return error(QStringLiteral("Usage: navigate providerDetails MODULE_ID"));
            args.insert(QStringLiteral("moduleId"), arguments.at(1));
        } else if (route == QStringLiteral("subtitleSettings")) {
            if (count(2, 2))
                args.insert(QStringLiteral("rowKey"), arguments.at(1));
        } else if (!count(1, 1)) {
            return error(QStringLiteral("This route does not accept a route argument"));
        }
    } else if (command == QStringLiteral("key") || command == QStringLiteral("library")
        || command == QStringLiteral("screenshot")) {
        if (!count(1, 1))
            return error(QStringLiteral("Command requires one argument"));
        const QString key = command == QStringLiteral("key") ? QStringLiteral("name")
            : command == QStringLiteral("library")           ? QStringLiteral("id")
                                                             : QStringLiteral("path");
        args.insert(key,
            command == QStringLiteral("screenshot") && host.isEmpty() ? QFileInfo(arguments.first()).absoluteFilePath()
                                                                      : arguments.first());
    } else if (command == QStringLiteral("text")) {
        if (!count(1, 1))
            return error(QStringLiteral("Usage: text TEXT | text --stdin"));
        QString text = arguments.first();
        if (text == QStringLiteral("--stdin")) {
            QFile input;
            if (!input.open(stdin, QIODevice::ReadOnly))
                return error(QStringLiteral("Cannot read text from stdin"));
            const QByteArray bytes = input.read(16385);
            if (bytes.size() > 16384)
                return error(QStringLiteral("Text exceeds the input limit"));
            text = QString::fromUtf8(bytes);
        }
        if (text.size() > 4096)
            return error(QStringLiteral("Text must be at most 4096 characters"));
        args.insert(QStringLiteral("text"), text);
    } else if (command == QStringLiteral("play")) {
        if (!count(1, 2) || (arguments.size() == 2 && arguments.last() != QStringLiteral("--from-start")))
            return error(QStringLiteral("Usage: play QUALIFIED_ID [--from-start]"));
        args.insert(QStringLiteral("id"), arguments.first());
        args.insert(QStringLiteral("fromStart"), arguments.size() == 2);
    } else if (command == QStringLiteral("seek") || command == QStringLiteral("preview")
        || command == QStringLiteral("quality")) {
        if (!count(1, 1))
            return error(QStringLiteral("Command requires one numeric argument"));
        bool valid = false;
        const double value = arguments.first().toDouble(&valid);
        if (!valid || !std::isfinite(value) || value < 0
            || (command == QStringLiteral("quality") && (value > 10000 || std::floor(value) != value)))
            return error(QStringLiteral("Invalid nonnegative numeric argument"));
        args.insert(command == QStringLiteral("quality") ? QStringLiteral("index") : QStringLiteral("seconds"), value);
    } else if (command == QStringLiteral("pointer")) {
        if (!count(3, 3)
            || !QStringList { QStringLiteral("move"), QStringLiteral("click"), QStringLiteral("press"),
                QStringLiteral("release"), QStringLiteral("right-click") }
                .contains(arguments.first()))
            return error(QStringLiteral("Usage: pointer move|click|press|release|right-click X Y"));
        args.insert(QStringLiteral("action"), arguments.first());
        for (int index = 1; index <= 2; ++index) {
            bool valid = false;
            const double value = arguments.at(index).toDouble(&valid);
            if (!valid || !std::isfinite(value) || value < 0)
                return error(QStringLiteral("Pointer coordinates must be finite and nonnegative"));
            args.insert(index == 1 ? QStringLiteral("x") : QStringLiteral("y"), value);
        }
    } else if (command == QStringLiteral("browse")) {
        if (!count(2, 3) || arguments.first() != QStringLiteral("sort")
            || !QRegularExpression(QStringLiteral("^[A-Za-z][A-Za-z0-9]{0,63}$")).match(arguments.at(1)).hasMatch()
            || (arguments.size() == 3 && arguments.last() != QStringLiteral("Ascending")
                && arguments.last() != QStringLiteral("Descending")))
            return error(QStringLiteral("Usage: browse sort SORT_KEY [Ascending|Descending]"));
        command = QStringLiteral("browse-sort");
        args.insert(QStringLiteral("sortBy"), arguments.at(1));
        args.insert(
            QStringLiteral("sortOrder"), arguments.size() == 3 ? arguments.last() : QStringLiteral("Ascending"));
    } else if (command == QStringLiteral("items")) {
        if (!count(0, 3))
            return error(QStringLiteral("Usage: items [KIND] [OFFSET] [LIMIT]"));
        if (!arguments.isEmpty())
            args.insert(QStringLiteral("kind"), arguments.first());
        for (int index = 1; index < arguments.size(); ++index) {
            bool valid = false;
            const int value = arguments.at(index).toInt(&valid);
            if (!valid || value < 0)
                return error(QStringLiteral("Item paging values must be nonnegative integers"));
            args.insert(index == 1 ? QStringLiteral("offset") : QStringLiteral("limit"), value);
        }
    } else if (command == QStringLiteral("downloads")) {
        if (arguments.isEmpty())
            return error(QStringLiteral("Usage: downloads list|options|start|cancel|retry|remove|play"));
        const QString action = arguments.takeFirst();
        args.insert(QStringLiteral("action"), action);
        if (action == QStringLiteral("list") && count(0, 0)) {
        } else if (action == QStringLiteral("options") && count(1, 1)) {
            args.insert(QStringLiteral("itemId"), arguments.first());
        } else if (action == QStringLiteral("start") && count(2, 2)) {
            bool valid = false;
            const int index = arguments.last().toInt(&valid);
            if (!valid || index < 0 || index > 10000)
                return error(QStringLiteral("Download option index must be a nonnegative integer"));
            args.insert(QStringLiteral("itemId"), arguments.first());
            args.insert(QStringLiteral("index"), index);
        } else if (QStringList { QStringLiteral("cancel"), QStringLiteral("retry"), QStringLiteral("remove"),
                       QStringLiteral("play") }
                       .contains(action)
            && count(1, 1)) {
            args.insert(QStringLiteral("jobId"), arguments.first());
        } else {
            return error(QStringLiteral(
                "Usage: downloads list | options ITEM_ID | start ITEM_ID INDEX | cancel|retry|remove|play JOB_ID"));
        }
    } else if (command == QStringLiteral("open")) {
        bool valid = false;
        const int index = arguments.size() == 2 ? arguments.last().toInt(&valid) : -1;
        if (!valid || index < 0)
            return error(QStringLiteral("Usage: open browse|resume|next-up|context|similar INDEX"));
        args.insert(QStringLiteral("source"), arguments.first());
        args.insert(QStringLiteral("index"), index);
    } else if (command == QStringLiteral("item-state")) {
        if (arguments.size() != 1 && arguments.size() != 3)
            return error(
                QStringLiteral("Usage: item-state QUALIFIED_ID [played|favorite true|false | position SECONDS]"));
        args.insert(QStringLiteral("id"), arguments.first());
        if (arguments.size() == 3) {
            const QString field = arguments.at(1);
            const QString value = arguments.at(2);
            args.insert(QStringLiteral("field"), field);
            if (field == QStringLiteral("position")) {
                bool valid = false;
                const double seconds = value.toDouble(&valid);
                if (!valid || !std::isfinite(seconds) || seconds < 0)
                    return error(QStringLiteral("Position must be nonnegative seconds"));
                args.insert(QStringLiteral("value"), seconds);
            } else if (value == QStringLiteral("true") || value == QStringLiteral("false")) {
                args.insert(QStringLiteral("value"), value == QStringLiteral("true"));
            } else {
                return error(QStringLiteral("played and favorite take true or false"));
            }
        }
    } else if (command == QStringLiteral("capture")) {
        const QString action = arguments.isEmpty() ? QString() : arguments.takeFirst();
        if (action == QStringLiteral("start") && count(1, 3)) {
            command = QStringLiteral("capture-start");
            args.insert(QStringLiteral("dir"), arguments.first());
            for (int index = 1; index < arguments.size(); ++index) {
                bool valid = false;
                const int value = arguments.at(index).toInt(&valid);
                if (!valid || value <= 0)
                    return error(QStringLiteral("Capture timings are positive milliseconds"));
                args.insert(index == 1 ? QStringLiteral("settleMs") : QStringLiteral("maxMs"), value);
            }
        } else if ((action == QStringLiteral("status") || action == QStringLiteral("stop")) && count(0, 0)) {
            command = QStringLiteral("capture-") + action;
        } else {
            return error(
                QStringLiteral("Usage: capture start ABS_DIR [SETTLE_MS] [MAX_MS] | capture status | capture stop"));
        }
    } else if (command == QStringLiteral("settings")) {
        if (arguments.isEmpty())
            return error(QStringLiteral("Usage: settings get [KEY] or settings set KEY JSON_VALUE"));
        const QString action = arguments.takeFirst();
        if (action == QStringLiteral("get") && count(0, 1)) {
            command = QStringLiteral("settings-get");
            if (!arguments.isEmpty())
                args.insert(QStringLiteral("key"), arguments.first());
        } else if (action == QStringLiteral("set") && count(2, 2)) {
            command = QStringLiteral("settings-set");
            args.insert(QStringLiteral("key"), arguments.first());
            QJsonParseError parseError;
            const QJsonDocument value = QJsonDocument::fromJson(
                (QStringLiteral("[") + arguments.last() + QLatin1Char(']')).toUtf8(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !value.isArray() || value.array().size() != 1)
                return error(QStringLiteral("Setting value must be valid JSON (strings need double quotes)"));
            args.insert(QStringLiteral("value"), value.array().first());
        } else {
            return error(QStringLiteral("Usage: settings get [KEY] or settings set KEY JSON_VALUE"));
        }
    } else if (!QStringList { QStringLiteral("instances"), QStringLiteral("status"), QStringLiteral("state"),
                   QStringLiteral("home"), QStringLiteral("back"), QStringLiteral("load-more"), QStringLiteral("pause"),
                   QStringLiteral("resume"), QStringLiteral("stop"), QStringLiteral("qualities") }
                   .contains(command)
        || !arguments.isEmpty()) {
        return error(QStringLiteral("Unknown command or extra arguments; see spoolet help"));
    }
    if (!host.isEmpty()) {
        if (command == QStringLiteral("instances") && !descriptorPath.isEmpty())
            return error(QStringLiteral("instances does not accept an explicit descriptor"));
        QFile script(QStringLiteral(":/automation/RemoteDiscovery.sh"));
        if (!script.open(QIODevice::ReadOnly))
            return error(QStringLiteral("Remote discovery script is unavailable"));
        const auto quote = [](QString value) {
            value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
            return QLatin1Char('\'') + value + QLatin1Char('\'');
        };
        QElapsedTimer clock;
        clock.start();
        const auto remaining = [&] { return qMax(0, timeout - int(clock.elapsed())); };
        QString discoveryError;
        const QByteArray output = Spool::LocalControl::sshExchange(host,
            QStringLiteral("sh -s -- descriptors %1 %2").arg(quote(instance), quote(descriptorPath)), script.readAll(),
            remaining(), 64 * 4097, &discoveryError);
        if (!discoveryError.isEmpty())
            return error(discoveryError);
        QList<QJsonObject> descriptors;
        for (const QByteArray& line : output.split('\n')) {
            if (line.trimmed().isEmpty())
                continue;
            const QJsonObject row = QJsonDocument::fromJson(line).object();
            const int port = row.value(QStringLiteral("port")).toInt();
            const QString id = row.value(QStringLiteral("instance")).toString();
            if (line.size() > 4096 || !Spool::LocalControl::validInstance(id) || (!instance.isEmpty() && id != instance)
                || row.value(QStringLiteral("transport")).toString() != QStringLiteral("tcp")
                || row.value(QStringLiteral("endpoint")).toString() != QStringLiteral("127.0.0.1") || port < 1
                || port > 65535 || row.value(QStringLiteral("token")).toString().size() != 64)
                return error(QStringLiteral("Remote discovery contains an invalid loopback descriptor"));
            descriptors.append(row);
            if (descriptors.size() > 64)
                return error(QStringLiteral("Too many remote instances; use --instance"));
        }
        if (descriptors.isEmpty())
            return error(QStringLiteral("No private remote descriptors found; launch with SPOOL_AUTOMATION_PORT"));
        QList<QJsonObject> live;
        QJsonArray instances;
        for (const QJsonObject& row : descriptors) {
            if (remaining() <= 0)
                return error(QStringLiteral("Remote discovery timed out"));
            const QJsonObject reply
                = Spool::LocalControl::requestSsh(host, row, QStringLiteral("status"), {}, remaining());
            if (reply.value(QStringLiteral("ok")).toBool()) {
                live.append(row);
                instances.append(reply.value(QStringLiteral("result")));
            } else if (!instance.isEmpty() || !descriptorPath.isEmpty()) {
                return print(reply);
            }
        }
        if (command == QStringLiteral("instances"))
            return print({ { QStringLiteral("ok"), true },
                { QStringLiteral("result"), QJsonObject { { QStringLiteral("instances"), instances } } } });
        if (live.size() != 1)
            return error(live.isEmpty() ? QStringLiteral("No reachable remote instances found")
                                        : QStringLiteral("Multiple remote instances; use --instance ID"));
        if (remaining() <= 0)
            return error(QStringLiteral("Remote command timed out"));
        return print(Spool::LocalControl::requestSsh(host, live.first(), command, args, remaining()));
    }
    if (!descriptorPath.isEmpty()) {
        const QJsonObject descriptor = Spool::LocalControl::readDescriptor(descriptorPath);
        if (descriptor.isEmpty())
            return error(QStringLiteral("Explicit descriptor is absent or unsafe"));
        if (command == QStringLiteral("instances"))
            return error(QStringLiteral("instances does not accept an explicit descriptor"));
        return print(Spool::LocalControl::request(descriptor, command, args, timeout));
    }
    QString directoryError;
    const QString directory = Spool::LocalControl::directory(&directoryError);
    if (directory.isEmpty())
        return error(directoryError);
    QList<QJsonObject> descriptors;
    if (!instance.isEmpty()) {
        const QJsonObject descriptor
            = Spool::LocalControl::readDescriptor(QDir(directory).filePath(instance + QStringLiteral(".json")));
        if (descriptor.isEmpty())
            return error(QStringLiteral("Instance discovery is absent or unsafe"));
        descriptors.append(descriptor);
    } else {
        const QStringList files = QDir(directory).entryList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);
        if (files.size() > 64)
            return error(QStringLiteral("Too many discovery entries; specify --instance explicitly"));
        for (const QString& file : files) {
            const QJsonObject descriptor = Spool::LocalControl::readDescriptor(QDir(directory).filePath(file));
            if (!descriptor.isEmpty())
                descriptors.append(descriptor);
        }
    }
    if (command == QStringLiteral("instances") || instance.isEmpty()) {
        QList<QJsonObject> live;
        QJsonArray rows;
        for (const QJsonObject& descriptor : descriptors) {
            const QJsonObject reply
                = Spool::LocalControl::request(descriptor, QStringLiteral("status"), {}, qMin(timeout, 500));
            if (reply.value(QStringLiteral("ok")).toBool()) {
                live.append(descriptor);
                rows.append(reply.value(QStringLiteral("result")));
            }
        }
        if (command == QStringLiteral("instances"))
            return print({ { QStringLiteral("ok"), true },
                { QStringLiteral("result"), QJsonObject { { QStringLiteral("instances"), rows } } } });
        descriptors = live;
    }
    if (descriptors.isEmpty())
        return error(QStringLiteral("No running Spool instances found"));
    if (descriptors.size() != 1)
        return error(QStringLiteral("Multiple instances are running; use --instance ID"));
    return print(Spool::LocalControl::request(descriptors.first(), command, args, timeout));
}
