#include "AndroidLocalMediaSession.h"

#include "app/ArtworkService.h"
#include "platform/PlatformApplicationServices.h"
#include "player/PlayQueueController.h"
#include "player/PlayerController.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QQuickImageResponse>
#include <QQuickTextureFactory>
#include <QtCore/qnativeinterface.h>

namespace JellyfinNative {
namespace {
    // Only accessed on the Qt application thread, including JNI dispatch below.
    AndroidLocalMediaSession *localMediaSession = nullptr;
    constexpr auto serviceClass = "com/sachk/spool/LocalMediaPlaybackService";
}

AndroidLocalMediaSession::AndroidLocalMediaSession(ApplicationHooks& hooks)
    : m_hooks(hooks)
    , m_player(*hooks.player)
    , m_queue(*hooks.playQueue)
{
    localMediaSession = this;
    // End-of-file first clears PlayerController, then the app negotiates the
    // successor. Observe the settled transition, not that intermediate empty player.
    connect(&m_player, &PlayerController::sessionActiveChanged, this, &AndroidLocalMediaSession::update,
        Qt::QueuedConnection);
    connect(&m_player, &PlayerController::playbackStateChanged, this, &AndroidLocalMediaSession::update,
        Qt::QueuedConnection);
    connect(
        &m_player, &PlayerController::positionChanged, this, &AndroidLocalMediaSession::update, Qt::QueuedConnection);
    connect(&m_player, &PlayerController::effectivePlaybackSpeedChanged, this, &AndroidLocalMediaSession::update,
        Qt::QueuedConnection);
    connect(
        &m_queue, &PlayQueueController::queueChanged, this, &AndroidLocalMediaSession::update, Qt::QueuedConnection);
    connect(&hooks, &ApplicationHooks::playbackTransitionChanged, this, &AndroidLocalMediaSession::update,
        Qt::QueuedConnection);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &AndroidLocalMediaSession::clear);
    update();
}

AndroidLocalMediaSession::~AndroidLocalMediaSession()
{
    localMediaSession = nullptr;
    clear();
}

void AndroidLocalMediaSession::update()
{
    const bool active = m_player.sessionActive();
    const MovieItem item = m_queue.currentItem();
    const bool continuing = m_active && m_hooks.playbackTransition() && item.itemType == QStringLiteral("Audio");
    if ((!active && !continuing) || (active && m_player.mediaKind() != QStringLiteral("audio"))) {
        clear();
        return;
    }
    if (active && m_transitionPaused.has_value()) {
        const bool paused = *m_transitionPaused;
        m_transitionPaused.reset();
        m_player.setPaused(paused);
    }

    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid())
        return;
    m_active = true;
    const QJniObject title = QJniObject::fromString(active ? m_player.title() : item.title);
    const QJniObject artist = QJniObject::fromString(item.albumArtist);
    const QJniObject album = QJniObject::fromString(item.album);
    QJniObject::callStaticMethod<void>(serviceClass, "update",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJZZDZZ)V",
        context.object<jobject>(), title.object<jstring>(), artist.object<jstring>(), album.object<jstring>(),
        static_cast<jlong>(active ? m_player.durationSeconds() * 1000.0 : 0),
        static_cast<jlong>(active ? m_player.estimatedPositionSeconds() * 1000.0 : 0),
        static_cast<jboolean>(active ? !m_player.paused() : !m_transitionPaused.value_or(false)),
        static_cast<jboolean>(!active || m_player.buffering()), static_cast<jdouble>(m_player.effectivePlaybackSpeed()),
        static_cast<jboolean>(m_queue.canGoNext()), static_cast<jboolean>(m_queue.canGoPrevious()));
    if (auto *artwork = m_hooks.artwork)
        updateArtwork(artwork->itemUrl(item, false, 512));
}

void AndroidLocalMediaSession::updateArtwork(const QString& url)
{
    if (url == m_artworkUrl)
        return;
    m_artworkUrl = url;
    if (m_artworkResponse) {
        m_artworkResponse->cancel();
        m_artworkResponse->deleteLater();
        m_artworkResponse = nullptr;
    }
    // Clear the previous album immediately; a slow response must not put it
    // back after the queue has moved on. Reuse the authenticated artwork cache.
    QJniObject::callStaticMethod<void>(serviceClass, "setArtwork", "([B)V", static_cast<jbyteArray>(nullptr));
    if (url.isEmpty())
        return;
    auto *response
        = m_hooks.artwork->requestImageResponse(QString::fromLatin1(QUrl::toPercentEncoding(url)), QSize(512, 512));
    m_artworkResponse = response;
    connect(response, &QQuickImageResponse::finished, this, [this, response]() {
        response->deleteLater();
        if (!m_active || m_artworkResponse != response)
            return;
        m_artworkResponse = nullptr;
        const std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
        if (!texture || !response->errorString().isEmpty())
            return;
        QByteArray encoded;
        QBuffer buffer(&encoded);
        buffer.open(QIODevice::WriteOnly);
        if (!texture->image().save(&buffer, "PNG"))
            return;
        QJniEnvironment env;
        jbyteArray bytes = env->NewByteArray(static_cast<jsize>(encoded.size()));
        if (!bytes)
            return;
        env->SetByteArrayRegion(
            bytes, 0, static_cast<jsize>(encoded.size()), reinterpret_cast<const jbyte *>(encoded.constData()));
        QJniObject::callStaticMethod<void>(serviceClass, "setArtwork", "([B)V", bytes);
        env->DeleteLocalRef(bytes);
    });
}

void AndroidLocalMediaSession::clear()
{
    if (!m_active)
        return;
    m_active = false;
    m_transitionPaused.reset();
    updateArtwork({});
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (context.isValid())
        QJniObject::callStaticMethod<void>(
            serviceClass, "clear", "(Landroid/content/Context;)V", context.object<jobject>());
}

void AndroidLocalMediaSession::setPaused(bool paused)
{
    if (m_player.sessionActive())
        m_player.setPaused(paused);
    else
        m_transitionPaused = paused;
    // A system pause or audio-focus loss during successor negotiation must also
    // pause the incoming track, not disappear into an inactive PlayerController.
    update();
}

void AndroidLocalMediaSession::control(int action, qint64 value)
{
    if (!m_active)
        return;
    switch (action) {
    case 0:
        setPaused(false);
        break;
    case 1:
        setPaused(true);
        break;
    case 2:
        setPaused(m_player.sessionActive() ? !m_player.paused() : !m_transitionPaused.value_or(false));
        break;
    case 3:
        m_hooks.stopPlayback();
        clear();
        break;
    case 4:
        m_hooks.playNext();
        break;
    case 5:
        m_hooks.playPrevious();
        break;
    case 6:
        m_player.seek(static_cast<double>(value) / 1000.0);
        break;
    default:
        break;
    }
}

void dispatchAndroidLocalMediaControl(int action, qint64 value)
{
    if (QCoreApplication *application = QCoreApplication::instance()) {
        QMetaObject::invokeMethod(
            application,
            [action, value]() {
                if (localMediaSession)
                    localMediaSession->control(action, value);
            },
            Qt::QueuedConnection);
    }
}

} // namespace JellyfinNative

extern "C" JNIEXPORT void JNICALL Java_com_sachk_spool_LocalMediaPlaybackService_nativeControl(
    JNIEnv *, jclass, jint action, jlong value)
{
    JellyfinNative::dispatchAndroidLocalMediaControl(static_cast<int>(action), static_cast<qint64>(value));
}
