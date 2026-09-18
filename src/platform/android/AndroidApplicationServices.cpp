#include "platform/PlatformApplicationServices.h"

#include "player/PlayerController.h"

#include <QGuiApplication>
#include <QJniObject>

namespace JellyfinNative {

struct PlatformApplicationServices::PlatformData {
    explicit PlatformData(ApplicationHooks& applicationHooks)
        : hooks(&applicationHooks)
        , player(applicationHooks.player)
    {
    }

    ApplicationHooks *hooks = nullptr;
    PlayerController *player = nullptr;
};

PlatformApplicationServices::PlatformApplicationServices(
    QGuiApplication&, NativeAppWindow&, ApplicationHooks& hooks, RouterController&)
    : m_platform(std::make_unique<PlatformData>(hooks))
{
}

PlatformApplicationServices::~PlatformApplicationServices() = default;

void PlatformApplicationServices::start()
{
    PlatformData *platform = m_platform.get();
    QObject::connect(
        qGuiApp, &QGuiApplication::applicationStateChanged, qGuiApp, [platform](Qt::ApplicationState state) {
            if (!platform->player)
                return;
            if (state == Qt::ApplicationActive) {
                platform->player->resyncForForeground();
            } else if (state == Qt::ApplicationSuspended || state == Qt::ApplicationHidden) {
                if (platform->player->sessionActive() && !platform->player->paused())
                    platform->player->setPaused(true);
            }
        });

    QObject::connect(
        platform->hooks, &ApplicationHooks::diagnosticsReportSaved, qGuiApp, [platform](const QString& reportPath) {
            const QJniObject context = QNativeInterface::QAndroidApplication::context();
            const QJniObject path = QJniObject::fromString(reportPath);
            const jboolean opened = QJniObject::callStaticMethod<jboolean>("com/sachk/spool/AndroidUpdateBridge",
                "shareDiagnostics", "(Landroid/content/Context;Ljava/lang/String;)Z", context.object<jobject>(),
                path.object<jstring>());
            if (!opened)
                emit platform->hooks->toastRequested(QStringLiteral("Could not open Android’s share menu."));
        });
}

} // namespace JellyfinNative
