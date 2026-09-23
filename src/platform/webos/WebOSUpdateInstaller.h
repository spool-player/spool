#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QProcess;

namespace Spool {

class WebOSUpdateInstaller final : public QObject {
    Q_OBJECT

public:
    explicit WebOSUpdateInstaller(QObject *parent = nullptr);

    bool isRunning() const;
    void install(const QString& packagePath);

signals:
    void finished();
    void failed(const QString& message);
    void statusChanged(const QString& message);

private:
    void readStandardOutput(bool finalChunk = false);
    void readStandardError();
    void handleResponse(const QByteArray& line);
    void complete(bool success, const QString& message);
    QString processFailure(const QString& message) const;

    QProcess *m_process = nullptr;
    QByteArray m_output;
    QByteArray m_errorOutput;
};

} // namespace Spool
