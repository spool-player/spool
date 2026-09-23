#include "platform/PlatformApplicationServices.h"

#include "app/RouterController.h"
#include "app/SettingsController.h"
#include "platform/NativeAppWindow.h"
#include "platform/webos/WebOSAudioRoute.h"
#include "platform/webos/WebOSMpvRuntime.h"
#include "player/PlayerController.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QQuickWindow>
#include <QTimer>

extern "C" {
#include <luna-service2/lunaservice.h>
#include <wayland-webos-shell-client-protocol.h>
#include <webos-helpers/libhelpers.h>
}

namespace Spool {

struct PlatformApplicationServices::PlatformData {
    PlatformData(QGuiApplication& guiApplication, NativeAppWindow& nativeWindow, ApplicationHooks& applicationHooks,
        RouterController& routerController)
        : application(&guiApplication)
        , window(&nativeWindow)
        , hooks(&applicationHooks)
        , router(&routerController)
    {
        backgroundTrimTimer.setSingleShot(true);
        backgroundTrimTimer.setInterval(1000);
    }

    ~PlatformData()
    {
        unregister(lifecycleContext);
        unregister(imeContext);
        unregister(memoryContext);
        unregister(soundOutputContext);
    }

    static void unregister(std::unique_ptr<HContext>& context)
    {
        if (!context)
            return;
        HUnregisterServiceCallback(context.get());
        context.reset();
    }

    bool subscribe(std::unique_ptr<HContext>& destination, const char *uri, const char *payload,
        bool (*callback)(LSHandle *, LSMessage *, void *))
    {
        auto context = std::make_unique<HContext>();
        context->pub = true;
        context->multiple = true;
        context->callback = callback;
        context->userdata = this;
        if (HLunaServiceCall(uri, payload, context.get())) {
            qWarning() << "webOS service registration failed" << uri;
            return false;
        }
        destination = std::move(context);
        return true;
    }

    static PlatformData *fromCallbackContext(void *data)
    {
        // libhelpers passes the HContext itself as LSFilterFunc's ctx argument,
        // not HContext::userdata.
        auto *context = static_cast<HContext *>(data);
        return context ? static_cast<PlatformData *>(context->userdata) : nullptr;
    }

    static bool lifecycle(LSHandle *, LSMessage *message, void *data)
    {
        auto *platform = fromCallbackContext(data);
        const QByteArray payload(message && LSMessageGetPayload(message) ? LSMessageGetPayload(message) : "");
        const QJsonDocument document = QJsonDocument::fromJson(payload);
        if (!platform || !document.isObject())
            return true;
        const QJsonObject object = document.object();
        const QString event = object.value(QStringLiteral("event")).toString();
        if (event == QStringLiteral("relaunch") && platform->window) {
            QMetaObject::invokeMethod(
                platform->window, [platform] { platform->window->bringToFront(); }, Qt::QueuedConnection);
        } else if (event == QStringLiteral("close") && platform->application) {
            const QString reason = object.value(QStringLiteral("reason")).toString();
            QMetaObject::invokeMethod(
                platform->application,
                [platform, reason] {
                    if (reason == QStringLiteral("memoryReclaim") && platform->router)
                        platform->router->requestRecoveryOnNextLaunch(reason);
                    platform->application->quit();
                },
                Qt::QueuedConnection);
        }
        return true;
    }

    static bool memoryStatus(LSHandle *, LSMessage *message, void *data)
    {
        auto *platform = fromCallbackContext(data);
        const QByteArray payload(message && LSMessageGetPayload(message) ? LSMessageGetPayload(message) : "");
        const QJsonDocument document = QJsonDocument::fromJson(payload);
        if (!platform || !platform->hooks || !document.isObject())
            return true;
        const QJsonObject object = document.object();
        if (!object.value(QStringLiteral("returnValue")).toBool(true)
            && object.value(QStringLiteral("errorText"))
                .toString()
                .contains(QStringLiteral("Service does not exist"))) {
            return true;
        }
        const QString level = object.value(QStringLiteral("level")).toString();
        if (!level.isEmpty()) {
            QMetaObject::invokeMethod(
                platform->hooks,
                [platform, level] {
                    if (platform->hooks->memoryPressure)
                        platform->hooks->memoryPressure(level);
                },
                Qt::QueuedConnection);
        }
        return true;
    }

    static bool soundOutput(LSHandle *, LSMessage *message, void *data)
    {
        auto *platform = fromCallbackContext(data);
        if (platform && message && LSMessageGetPayload(message))
            platform->audioRoute.acceptServicePayload(QByteArray(LSMessageGetPayload(message)));
        return true;
    }

    static bool remoteKeyboard(LSHandle *, LSMessage *message, void *)
    {
        if (message && LSMessageGetPayload(message))
            qInfo() << "webOS remote keyboard" << LSMessageGetPayload(message);
        return true;
    }

    QGuiApplication *application = nullptr;
    NativeAppWindow *window = nullptr;
    ApplicationHooks *hooks = nullptr;
    RouterController *router = nullptr;
    WebOSAudioRoute audioRoute;
    QTimer backgroundTrimTimer;
    std::unique_ptr<HContext> lifecycleContext;
    std::unique_ptr<HContext> imeContext;
    std::unique_ptr<HContext> memoryContext;
    std::unique_ptr<HContext> soundOutputContext;
    bool started = false;
};

PlatformApplicationServices::PlatformApplicationServices(
    QGuiApplication& application, NativeAppWindow& window, ApplicationHooks& hooks, RouterController& router)
    : m_platform(std::make_unique<PlatformData>(application, window, hooks, router))
{
}

PlatformApplicationServices::~PlatformApplicationServices() = default;

void PlatformApplicationServices::start()
{
    if (m_platform->started)
        return;
    m_platform->started = true;

    QObject::connect(&m_platform->audioRoute, &WebOSAudioRoute::routeChanged, m_platform->hooks->settings,
        [settings = m_platform->hooks->settings](const QString& output, int displayLatencyMs, int outputLatencyMs) {
            settings->updateAudioOutputRoute(output, displayLatencyMs, outputLatencyMs);
        });
    QObject::connect(
        m_platform->hooks, &ApplicationHooks::aggressiveMemoryPressure, m_platform->window,
        [window = m_platform->window] { window->releaseResources(); }, Qt::QueuedConnection);
    QObject::connect(
        &m_platform->backgroundTrimTimer, &QTimer::timeout, m_platform->hooks, [hooks = m_platform->hooks] {
            if (hooks->memoryPressure)
                hooks->memoryPressure(QStringLiteral("critical"));
        });
    QObject::connect(m_platform->application, &QGuiApplication::applicationStateChanged, m_platform->application,
        [platform = m_platform.get()](Qt::ApplicationState state) {
            PlayerController *player = platform->hooks->player;
            if (state == Qt::ApplicationHidden || state == Qt::ApplicationSuspended)
                player->teardownForBackground();
            else if (state == Qt::ApplicationInactive)
                player->prepareForBackground();
            else if (state == Qt::ApplicationActive)
                player->resyncForForeground();
        });
    QObject::connect(m_platform->application, &QGuiApplication::applicationStateChanged,
        &m_platform->backgroundTrimTimer, [platform = m_platform.get()](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive)
                platform->backgroundTrimTimer.stop();
            else
                platform->backgroundTrimTimer.start();
        });
    QObject::connect(m_platform->window, &NativeAppWindow::platformSurfaceStateChanged, m_platform->application,
        [platform = m_platform.get()](int state) {
            PlayerController *player = platform->hooks->player;
            if (state == WL_WEBOS_SHELL_SURFACE_STATE_MINIMIZED) {
                if (player->sessionActive() && !player->paused()) {
                    qInfo() << "webOS shell minimized: pausing playback";
                    player->setPaused(true);
                }
                player->prepareForBackground();
                platform->backgroundTrimTimer.start();
            } else if (state == WL_WEBOS_SHELL_SURFACE_STATE_MAXIMIZED
                || state == WL_WEBOS_SHELL_SURFACE_STATE_FULLSCREEN) {
                platform->backgroundTrimTimer.stop();
                player->resyncForForeground();
            }
        });
    QObject::connect(m_platform->window, &NativeAppWindow::platformSurfaceExposed, &m_platform->backgroundTrimTimer,
        [platform = m_platform.get()](bool exposed) {
            PlayerController *player = platform->hooks->player;
            if (exposed) {
                platform->backgroundTrimTimer.stop();
                player->resyncForForeground();
            } else {
                player->teardownForBackground();
                platform->backgroundTrimTimer.start();
            }
        });
    QObject::connect(m_platform->window, &NativeAppWindow::platformCloseRequested, m_platform->application,
        [platform = m_platform.get()] {
            platform->router->requestRecoveryOnNextLaunch(QStringLiteral("shellClose"));
            platform->application->quit();
        });
    QObject::connect(
        m_platform->window, &QQuickWindow::frameSwapped, m_platform->window,
        [] {
            static bool started = false;
            if (!started) {
                started = true;
                WebOSMpvRuntime::preloadAsync();
            }
        },
        Qt::QueuedConnection);

    m_platform->subscribe(m_platform->lifecycleContext, "luna://com.webos.service.applicationmanager/registerApp", "{}",
        &PlatformData::lifecycle);
    m_platform->subscribe(m_platform->imeContext, "luna://com.webos.service.ime/registerRemoteKeyboard",
        "{\"subscribe\":true}", &PlatformData::remoteKeyboard);
    m_platform->subscribe(m_platform->memoryContext, "luna://com.webos.service.memorymanager/getMemoryStatus",
        "{\"subscribe\":true}", &PlatformData::memoryStatus);
    m_platform->subscribe(m_platform->soundOutputContext, "luna://com.webos.service.audio/getSoundOutput",
        "{\"subscribe\":true}", &PlatformData::soundOutput);
}

} // namespace Spool
