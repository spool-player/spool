#include "LocalControlProtocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
            QJsonObject {
                { QStringLiteral("usage"),
                    QStringLiteral("spoolet [--instance ID | --descriptor FILE] [--timeout MS] COMMAND [ARGS]") },
                { QStringLiteral("commands"),
                    QJsonArray { QStringLiteral("help | instances | status | state"),
                        QStringLiteral(
                            "navigate home|search|settings | home | back | key up|down|left|right|ok|back|space"),
                        QStringLiteral("items [browse|libraries|resume|next-up] [OFFSET] [LIMIT] | library "
                                       "QUALIFIED_ID | load-more"),
                        QStringLiteral("play QUALIFIED_ID [--from-start] | pause | resume | stop | seek SECONDS"),
                        QStringLiteral("preview SECONDS (actual timeline hover; key up first if controls are hidden)"),
                        QStringLiteral("qualities | quality INDEX"), QStringLiteral("screenshot FILE.png"),
                        QStringLiteral("pointer move|click|press|release|right-click X Y (window logical coordinates)"),
                        QStringLiteral("text TEXT | text --stdin (commit to the focused editable field; no echo)"),
                        QStringLiteral(
                            "downloads list | options ITEM_ID | start ITEM_ID INDEX | cancel|retry|remove|play JOB_ID"),
                        QStringLiteral("settings get [KEY] | settings set KEY JSON_VALUE") } },
                { QStringLiteral("notes"),
                    QStringLiteral(
                        "All stdout is JSON. Commands are bounded and never prompt. When multiple instances run, "
                        "--instance is required. Playback/navigation acceptance is not a promise that provider I/O has "
                        "completed: poll status. Screenshot waits for a new Qt frame, then atomically writes PNG; "
                        "separate native video planes are reported as excluded.") } } } };
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("spoolet"));
    QStringList arguments = app.arguments().mid(1);
    QString instance;
    QString descriptorPath;
    int timeout = Spool::LocalControl::RequestTimeoutMs;
    while (!arguments.isEmpty() && arguments.first().startsWith(QStringLiteral("--"))) {
        const QString option = arguments.takeFirst();
        if (option == QStringLiteral("--help"))
            return print(help());
        if ((option != QStringLiteral("--instance") && option != QStringLiteral("--timeout")
                && option != QStringLiteral("--descriptor"))
            || arguments.isEmpty())
            return error(QStringLiteral("Expected --instance ID, --descriptor FILE or --timeout MS; see help"));
        const QString value = arguments.takeFirst();
        if (option == QStringLiteral("--instance")) {
            if (!Spool::LocalControl::validInstance(value))
                return error(QStringLiteral("Invalid instance identifier"));
            instance = value;
        } else if (option == QStringLiteral("--descriptor")) {
            descriptorPath = QFileInfo(value).absoluteFilePath();
        } else {
            bool valid = false;
            timeout = value.toInt(&valid);
            if (!valid || timeout < 100 || timeout > 30'000)
                return error(QStringLiteral("Timeout must be 100..30000 milliseconds"));
        }
    }
    if (!instance.isEmpty() && !descriptorPath.isEmpty())
        return error(QStringLiteral("--instance and --descriptor are mutually exclusive"));
    if (arguments.isEmpty() || arguments == QStringList { QStringLiteral("help") })
        return print(help());
    QString command = arguments.takeFirst();
    QJsonObject args;
    const auto count = [&](int low, int high) { return arguments.size() >= low && arguments.size() <= high; };
    if (command == QStringLiteral("navigate") || command == QStringLiteral("key")
        || command == QStringLiteral("library") || command == QStringLiteral("screenshot")) {
        if (!count(1, 1))
            return error(QStringLiteral("Command requires one argument"));
        const QString key = command == QStringLiteral("navigate") ? QStringLiteral("route")
            : command == QStringLiteral("key")                    ? QStringLiteral("name")
            : command == QStringLiteral("library")                ? QStringLiteral("id")
                                                                  : QStringLiteral("path");
        args.insert(key,
            command == QStringLiteral("screenshot") && descriptorPath.isEmpty()
                ? QFileInfo(arguments.first()).absoluteFilePath()
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
