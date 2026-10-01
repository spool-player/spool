// Static platform input context plugin exposing WebOSInputContext under the
// "webosim" key (selected via QT_IM_MODULE in main.cpp).
#include <qpa/qplatforminputcontextplugin_p.h>

#include <QtCore/QStringList>

#include "WebOSInputContext.h"

class WebOSPlatformInputContextPlugin : public QPlatformInputContextPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QPlatformInputContextFactoryInterface.5.1" FILE "webosim.json")

public:
    QPlatformInputContext *create(const QString& system, const QStringList& paramList) override;
};

QPlatformInputContext *WebOSPlatformInputContextPlugin::create(const QString& system, const QStringList& paramList)
{
    Q_UNUSED(paramList);
    if (system.compare(QStringLiteral("webosim"), Qt::CaseInsensitive) == 0)
        return new Spool::WebOSInputContext;
    return nullptr;
}

#include "WebOSInputContextPlugin.moc"
