#include "diagnostics/InputLatencyMonitor.h"
#include "platform/PlatformCapabilities.h"

#include "ExtensionIntegration.h"

#include <QDir>
#include <QFontDatabase>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QSettings>
#include <QTemporaryDir>
#include <QtQuickTest/quicktest.h>

class QmlTestSetup final : public QObject {
    Q_OBJECT

public slots:
    void qmlEngineAvailable(QQmlEngine *engine)
    {
        if (qEnvironmentVariableIsSet("SPOOL_EXTENSION_INTEGRATION")) {
            m_integration = new ExtensionIntegration(this);
            m_integration->expose(engine);
        }
    }

    void applicationAvailable()
    {
        const QDir fonts(QStringLiteral(TEST_SOURCE_DIR "/qml/fonts"));
        for (const auto& file : fonts.entryList({ "*.ttf", "*.otf" }, QDir::Files))
            if (QFontDatabase::addApplicationFont(fonts.filePath(file)) < 0)
                qFatal("test font registration failed");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, m_settings.path());
        m_latency = new Spool::InputLatencyMonitor(this);
        qmlRegisterSingletonInstance("Spool", 1, 0, "InputLatency", m_latency);
        const auto& capabilities = Spool::platformCapabilities();
        m_platform->insert(QStringLiteral("isTV"), capabilities.isTV);
        m_platform->insert(QStringLiteral("isWebOS"), capabilities.isWebOS);
        m_platform->insert(QStringLiteral("isAndroid"), capabilities.isAndroid);
        m_platform->insert(QStringLiteral("isMobile"), capabilities.isMobile);
        m_platform->insert(QStringLiteral("hasSystemFonts"), capabilities.hasSystemFonts);
        m_platform->insert(QStringLiteral("hasDesktopPointer"), capabilities.hasDesktopPointer);
        m_platform->insert(QStringLiteral("hasPointer"), capabilities.hasPointer);
        m_platform->insert(QStringLiteral("supportsMpvConfig"), capabilities.supportsMpvConfig);
        m_platform->insert(QStringLiteral("usesPerOutputAudioDelay"), capabilities.usesPerOutputAudioDelay);
        qmlRegisterSingletonInstance("Spool", 1, 0, "Platform", m_platform);
    }

private:
    ExtensionIntegration *m_integration = nullptr;
    QQmlPropertyMap *m_platform = QQmlPropertyMap::create(this);
    QTemporaryDir m_settings;
    Spool::InputLatencyMonitor *m_latency = nullptr;
};

QUICK_TEST_MAIN_WITH_SETUP(spool, QmlTestSetup)

#include "QmlTestRunner.moc"
