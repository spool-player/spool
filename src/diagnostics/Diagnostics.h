#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <memory>

namespace Spool::Diagnostics {

void initialize(const QString& appId, const QString& rootPath);
void shutdown();
void logEvent(const QString& category, const QString& event, QJsonObject data = {});
void setInstanceState(const QString& state, QJsonObject extra = {});
void dumpDiagnostics(const QString& reason);
QString supportReportPreview();
QString saveSupportReport();

class EventLoopWatchdog final : public QObject {
    Q_OBJECT

public:
    explicit EventLoopWatchdog(QObject *parent = nullptr);
    ~EventLoopWatchdog() override;
};

class Phase final {
public:
    Phase(QString category, QString name, QJsonObject data = {});
    ~Phase();

private:
    QString m_category;
    QString m_name;
    qint64 m_startedMs = 0;
    bool m_active = false;
};

class Task final {
public:
    Task(QString name, QJsonObject data = {});
    ~Task();

private:
    QString m_name;
    QString m_id;
    qint64 m_startedMs = 0;
    bool m_active = false;
};

class NetworkRequest final {
public:
    NetworkRequest(QString method, QString url);
    ~NetworkRequest();
    void finish(int statusCode, const QString& errorText = {});

private:
    QString m_id;
    QString m_method;
    QString m_url;
    qint64 m_startedMs = 0;
    bool m_finished = false;
};

} // namespace Spool::Diagnostics
