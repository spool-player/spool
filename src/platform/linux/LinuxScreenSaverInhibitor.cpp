#include "platform/ScreenSaverInhibitor.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDebug>

namespace Spool {
namespace {

    class LinuxScreenSaverBackend final : public ScreenSaverBackend {
    public:
        bool acquire() override
        {
            QDBusInterface screenSaver(QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("/ScreenSaver"),
                QStringLiteral("org.freedesktop.ScreenSaver"), QDBusConnection::sessionBus());
            const QDBusReply<quint32> reply = screenSaver.call(
                QStringLiteral("Inhibit"), QStringLiteral("Spool"), QStringLiteral("Media playback is active"));
            if (!reply.isValid()) {
                qWarning() << "screensaver: freedesktop inhibit failed" << reply.error().message();
                return false;
            }
            m_cookie = reply.value();
            return true;
        }

        bool release() override
        {
            if (m_cookie == 0)
                return true;
            QDBusInterface screenSaver(QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("/ScreenSaver"),
                QStringLiteral("org.freedesktop.ScreenSaver"), QDBusConnection::sessionBus());
            screenSaver.call(QStringLiteral("UnInhibit"), m_cookie);
            m_cookie = 0;
            return true;
        }

    private:
        quint32 m_cookie = 0;
    };

} // namespace

std::unique_ptr<ScreenSaverBackend> createPlatformScreenSaverBackend()
{
    return std::make_unique<LinuxScreenSaverBackend>();
}

} // namespace Spool
