#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <memory>

namespace Spool {

class WebOSAudioRoute final : public QObject {
    Q_OBJECT

public:
    explicit WebOSAudioRoute(QObject *parent = nullptr);
    ~WebOSAudioRoute() override;

    void acceptServicePayload(const QByteArray& payload);

signals:
    void routeChanged(const QString& output, int displayLatencyMs, int outputLatencyMs);

private:
    struct PlatformData;
    std::unique_ptr<PlatformData> m_platform;
};

} // namespace Spool
