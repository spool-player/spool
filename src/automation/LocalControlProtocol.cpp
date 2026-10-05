#include "LocalControlProtocol.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUuid>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Spool::LocalControl {
QString directory(QString *error)
{
#ifdef Q_OS_UNIX
    const QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
#else
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#endif
    if (base.isEmpty()) {
        *error = QStringLiteral("No user-local runtime directory is available");
        return {};
    }
    const QString path = QDir(base).filePath(QStringLiteral("spool-control"));
    if (!QFileInfo::exists(path) && !QDir().mkdir(path)) {
        *error = QStringLiteral("Cannot create the local control directory");
        return {};
    }
#ifdef Q_OS_UNIX
    struct stat info {};
    const QByteArray encoded = QFile::encodeName(path);
    if (lstat(encoded.constData(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != geteuid()
        || chmod(encoded.constData(), 0700) != 0) {
        *error = QStringLiteral("Local control directory is not a private owned directory");
        return {};
    }
#else
    if (QFileInfo(path).isSymLink() || !QFileInfo(path).isDir()) {
        *error = QStringLiteral("Local control directory is not a user directory");
        return {};
    }
#endif
    return path;
}

bool validInstance(const QString& instance)
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$"));
    return expression.match(instance).hasMatch();
}

bool privateFile(const QString& path)
{
#ifdef Q_OS_UNIX
    struct stat info {};
    const QByteArray encoded = QFile::encodeName(path);
    return lstat(encoded.constData(), &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == geteuid()
        && (info.st_mode & 0077) == 0;
#else
    return QFileInfo(path).isFile() && !QFileInfo(path).isSymLink();
#endif
}

QJsonObject readDescriptor(const QString& path)
{
    if (!privateFile(path))
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096)
        return {};
    const QJsonObject result = QJsonDocument::fromJson(file.readAll()).object();
    const QString instance = result.value(QStringLiteral("instance")).toString();
    const QString endpoint = result.value(QStringLiteral("endpoint")).toString();
    const QString token = result.value(QStringLiteral("token")).toString();
    if (!validInstance(instance) || token.size() != 64 || endpoint.isEmpty() || endpoint.size() > 256)
        return {};
#ifdef Q_OS_UNIX
    if (QFileInfo(endpoint).absolutePath() != QFileInfo(path).absolutePath()
        || !QFileInfo(endpoint).fileName().startsWith(QStringLiteral("s-")))
        return {};
#else
    if (!endpoint.startsWith(QStringLiteral("spool-control-")) || endpoint.contains(QLatin1Char('/'))
        || endpoint.contains(QLatin1Char('\\')))
        return {};
#endif
    return result;
}

QJsonObject failure(const QString& id, const QString& code, const QString& message)
{
    return { { QStringLiteral("id"), id }, { QStringLiteral("ok"), false },
        { QStringLiteral("error"),
            QJsonObject { { QStringLiteral("code"), code }, { QStringLiteral("message"), message } } } };
}

QJsonObject request(const QJsonObject& descriptor, const QString& command, const QJsonObject& arguments, int timeoutMs)
{
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject message { { QStringLiteral("id"), id }, { QStringLiteral("command"), command },
        { QStringLiteral("args"), arguments }, { QStringLiteral("token"), descriptor.value(QStringLiteral("token")) } };
    const QByteArray bytes = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > MaxRequestBytes)
        return failure(id, QStringLiteral("request_too_large"), QStringLiteral("Request exceeds 64 KiB"));
    QElapsedTimer elapsed;
    elapsed.start();
    const auto remaining = [&] { return qMax(0, timeoutMs - int(elapsed.elapsed())); };
    QLocalSocket socket;
    socket.setReadBufferSize(MaxResponseBytes + 1);
    socket.connectToServer(descriptor.value(QStringLiteral("endpoint")).toString());
    if (!socket.waitForConnected(remaining()))
        return failure(id, QStringLiteral("unavailable"), QStringLiteral("Instance is not reachable"));
    if (socket.write(bytes) != bytes.size())
        return failure(id, QStringLiteral("write_failed"), QStringLiteral("Could not send command"));
    while (socket.bytesToWrite() > 0) {
        if (remaining() <= 0 || !socket.waitForBytesWritten(remaining()))
            return failure(id, QStringLiteral("timeout"), QStringLiteral("Command write timed out"));
    }
    QByteArray reply;
    while (true) {
        reply += socket.readAll();
        if (reply.size() > MaxResponseBytes)
            return failure(id, QStringLiteral("response_too_large"), QStringLiteral("Response exceeds 1 MiB"));
        if (reply.contains('\n'))
            break;
        if (remaining() <= 0 || !socket.waitForReadyRead(remaining()))
            return failure(id, QStringLiteral("timeout"), QStringLiteral("Instance did not complete command in time"));
    }
    QJsonParseError parseError;
    const QJsonObject result = QJsonDocument::fromJson(reply.left(reply.indexOf('\n')), &parseError).object();
    if (parseError.error != QJsonParseError::NoError || result.value(QStringLiteral("id")).toString() != id
        || !result.value(QStringLiteral("ok")).isBool())
        return failure(
            id, QStringLiteral("invalid_response"), QStringLiteral("Invalid response or mismatched request ID"));
    return result;
}
}
