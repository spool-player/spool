#pragma once

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
QJsonObject request(const QJsonObject& descriptor, const QString& command, const QJsonObject& arguments,
    int timeoutMs = RequestTimeoutMs);
QJsonObject failure(const QString& id, const QString& code, const QString& message);
}
