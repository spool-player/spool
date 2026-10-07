#include "LocalControlProtocol.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QNetworkProxy>
#include <algorithm>
#include <QUuid>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#include <sys/un.h>
#endif

namespace Spool::LocalControl {
namespace {
QString privateDirectory(const QString& base, QString *error)
{
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
}

QString directory(QString *error)
{
    QString base = qEnvironmentVariable("SPOOL_DATA_HOME");
    if (!base.isEmpty()) {
        if (!QDir::isAbsolutePath(base)) {
            *error = QStringLiteral("The explicit data directory must be absolute");
            return {};
        }
        base = QDir(base).filePath(QStringLiteral("runtime"));
        if (!QDir().mkpath(base)) {
            *error = QStringLiteral("Cannot create the explicit runtime directory");
            return {};
        }
    } else {
#if defined(SPOOL_ANDROID) || defined(SPOOL_APPLE_MOBILE)
        base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
#elif defined(Q_OS_UNIX)
        base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
#else
        base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#endif
    }
    return privateDirectory(base, error);
}

QString localEndpoint(const QString& registryDirectory, const QString& instance, QString *error)
{
    const QString root = QFileInfo(registryDirectory).canonicalFilePath();
    if (root.isEmpty() || !validInstance(instance)) {
        *error = QStringLiteral("Cannot resolve the control registry or instance identifier");
        return {};
    }
    QByteArray identity = root.toUtf8();
    identity.append('\0');
    identity.append(instance.toUtf8());
    const QString suffix = QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(24));
#ifdef Q_OS_UNIX
    QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (!runtime.isEmpty()) {
        struct stat base {};
        if (!QDir::isAbsolutePath(runtime) || lstat(QFile::encodeName(runtime).constData(), &base) != 0
            || !S_ISDIR(base.st_mode) || base.st_uid != geteuid() || (base.st_mode & 0077) != 0) {
            *error = QStringLiteral("XDG_RUNTIME_DIR must be an absolute private owned directory");
            return {};
        }
    } else {
        runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    }
    const QString sockets = privateDirectory(runtime, error);
    if (sockets.isEmpty())
        return {};
    const QString endpoint = QDir(sockets).filePath(QStringLiteral("s-") + suffix);
    if (QFile::encodeName(endpoint).size() >= qsizetype(sizeof(sockaddr_un {}.sun_path))) {
        *error = QStringLiteral("The OS user runtime directory is too long for a UNIX socket; use a shorter private runtime directory");
        return {};
    }
    return endpoint;
#else
    return QStringLiteral("spool-control-") + suffix;
#endif
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
    if (result.value(QStringLiteral("transport")).toString() == QStringLiteral("tcp")) {
        const int port = result.value(QStringLiteral("port")).toInt();
        if (endpoint != QStringLiteral("127.0.0.1") || port < 1 || port > 65535)
            return {};
        return result;
    }
    if (result.contains(QStringLiteral("transport")))
        return {};
    QString error;
    const QString expected = localEndpoint(QFileInfo(path).absolutePath(), instance, &error);
#ifdef Q_OS_UNIX
    if (expected.isEmpty() || endpoint != expected)
        return {};
#else
    const QString prefix = expected + QLatin1Char('-');
    const QString nonce = endpoint.mid(prefix.size());
    if (expected.isEmpty() || !endpoint.startsWith(prefix) || nonce.size() != 32
        || std::any_of(nonce.cbegin(), nonce.cend(), [](QChar value) {
            return !((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'));
        }))
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
    const bool remote = descriptor.value(QStringLiteral("transport")).toString() == QStringLiteral("tcp");
    if (remote && (descriptor.value(QStringLiteral("endpoint")).toString() != QStringLiteral("127.0.0.1")
            || descriptor.value(QStringLiteral("port")).toInt() < 1
            || descriptor.value(QStringLiteral("port")).toInt() > 65535))
        return failure(id, QStringLiteral("invalid_descriptor"), QStringLiteral("TCP control must use IPv4 loopback"));
    const auto exchange = [&](auto& socket) -> QJsonObject {
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
            return failure(id, QStringLiteral("invalid_response"), QStringLiteral("Invalid response or mismatched request ID"));
        return result;
    };
    if (remote) {
        QTcpSocket socket;
        socket.setProxy(QNetworkProxy::NoProxy);
        socket.setReadBufferSize(MaxResponseBytes + 1);
        socket.connectToHost(QHostAddress::LocalHost, quint16(descriptor.value(QStringLiteral("port")).toInt()));
        return exchange(socket);
    }
    QLocalSocket socket;
    socket.setReadBufferSize(MaxResponseBytes + 1);
    socket.connectToServer(descriptor.value(QStringLiteral("endpoint")).toString());
    return exchange(socket);
}
}
