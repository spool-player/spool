#include "AndroidLocalMediaSession.h"

#include "app/AppController.h"
#include "player/PlayQueueController.h"
#include "player/PlayerController.h"

#include <QCoreApplication>
#include <QJniObject>
#include <QtCore/qnativeinterface.h>

namespace JellyfinNative {
namespace {
    // Only accessed on the Qt application thread, including JNI dispatch below.
    AndroidLocalMediaSession *localMediaSession = nullptr;
    constexpr auto serviceClass = "com/sachk/spool/LocalMediaPlaybackService";
}

AndroidLocalMediaSession::AndroidLocalMediaSession(AppController& controller)
    : m_controller(controller)
    , m_player(*controller.player())
    , m_queue(*controller.playQueue())
{
    localMediaSession = this;
    // End-of-file first clears PlayerController, then AppController negotiates the
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
    connect(&controller, &AppController::playbackTransitionChanged, this, &AndroidLocalMediaSession::update,
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
    const bool continuing
        = m_active && m_controller.property("playbackTransition").toBool() && item.itemType == QStringLiteral("Audio");
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
}

void AndroidLocalMediaSession::clear()
{
    if (!m_active)
        return;
    m_active = false;
    m_transitionPaused.reset();
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
        m_controller.stopPlayback();
        clear();
        break;
    case 4:
        m_controller.playQueueNext();
        break;
    case 5:
        m_controller.playQueuePrevious();
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
