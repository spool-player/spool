#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace Spool::LocalControl {
constexpr qsizetype MaxRequestBytes = 64 * 1024;
constexpr qsizetype MaxResponseBytes = 1024 * 1024;
constexpr int RequestTimeoutMs = 10'000;

// Discovery files are private capabilities, not public command endpoints.
QString directory(QString *error);
QString localEndpoint(const QString& registryDirectory, const QString& instance, QString *error);
bool validInstance(const QString& instance);
bool privateFile(const QString& path);
QJsonObject readDescriptor(const QString& path);
// SSH transports the capability over stdin only; no remote spoolet is required.
// Platforms built without Qt process support report SSH as unavailable; their
// local socket/TCP control protocol remains supported.
bool validSshHost(const QString& host);
QByteArray sshExchange(const QString& host, const QString& command, const QByteArray& input, int timeoutMs,
    qsizetype maxBytes, QString *error, int forwardPort = 0);
QJsonObject requestSsh(const QString& host, const QJsonObject& descriptor, const QString& command,
    const QJsonObject& arguments, int timeoutMs = RequestTimeoutMs);
QJsonObject request(const QJsonObject& descriptor, const QString& command, const QJsonObject& arguments,
    int timeoutMs = RequestTimeoutMs);
QJsonObject failure(const QString& id, const QString& code, const QString& message);
}
