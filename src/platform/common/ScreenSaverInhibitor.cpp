#include "platform/ScreenSaverInhibitor.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>

namespace Spool {

ScreenSaverInhibitor::ScreenSaverInhibitor()
    : ScreenSaverInhibitor(createPlatformScreenSaverBackend())
{
}

ScreenSaverInhibitor::ScreenSaverInhibitor(std::unique_ptr<ScreenSaverBackend> backend)
    : m_backend(std::move(backend))
{
}

ScreenSaverInhibitor::~ScreenSaverInhibitor()
{
    setInhibited(false);
}

void ScreenSaverInhibitor::setInhibited(bool inhibited)
{
    if (m_inhibited == inhibited || !m_backend)
        return;
    const bool changed = inhibited ? m_backend->acquire() : m_backend->release();
    if (!changed)
        return;
    m_inhibited = inhibited;
    qInfo() << "screensaver:" << (inhibited ? "inhibited for active playback" : "available while paused or idle");
}

bool ScreenSaverInhibitor::inhibited() const
{
    return m_inhibited;
}

bool screenSaverShouldBeInhibited(bool mediaSessionActive, bool paused, bool slideshowAdvancing)
{
    return (mediaSessionActive && !paused) || slideshowAdvancing;
}

QByteArray webOsScreenSaverResponsePayload(const QByteArray& requestPayload)
{
    const QJsonObject request = QJsonDocument::fromJson(requestPayload).object();
    if (request.value(QStringLiteral("state")).toString() != QStringLiteral("Active"))
        return {};
    const QJsonObject response { { QStringLiteral("clientName"), QStringLiteral("com.sachk.spool") },
        { QStringLiteral("ack"), false }, { QStringLiteral("timestamp"), request.value(QStringLiteral("timestamp")) } };
    return QJsonDocument(response).toJson(QJsonDocument::Compact);
}

} // namespace Spool
