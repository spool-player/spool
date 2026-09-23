#include "platform/PlatformRemoteMediaSession.h"

#include "platform/PlatformCapabilities.h"

#include <QPointer>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniObject>
#include <QtCore/qnativeinterface.h>
#endif

namespace Spool {
namespace {

    class NullRemoteMediaSession final : public PlatformRemoteMediaSession {
    public:
        using PlatformRemoteMediaSession::PlatformRemoteMediaSession;

        void update(const RemoteMediaSessionState&) override { }
        void clear() override { }
    };

#ifdef Q_OS_ANDROID
    QPointer<PlatformRemoteMediaSession> androidRemoteMediaSession;

    class AndroidRemoteMediaSession final : public PlatformRemoteMediaSession {
    public:
        explicit AndroidRemoteMediaSession(QObject *parent)
            : PlatformRemoteMediaSession(parent)
        {
            const QJniObject activity = QNativeInterface::QAndroidApplication::context();
            if (!activity.isValid())
                return;
            m_bridge = QJniObject(
                "com/sachk/spool/RemoteMediaSessionBridge", "(Landroid/app/Activity;)V", activity.object<jobject>());
            if (m_bridge.isValid())
                androidRemoteMediaSession = this;
        }

        ~AndroidRemoteMediaSession() override
        {
            if (m_bridge.isValid())
                m_bridge.callMethod<void>("release", "()V");
            if (androidRemoteMediaSession == this)
                androidRemoteMediaSession.clear();
        }

        void update(const RemoteMediaSessionState& state) override
        {
            if (!m_bridge.isValid())
                return;
            const QJniObject title = QJniObject::fromString(state.title);
            const QJniObject artist = QJniObject::fromString(state.artist);
            const QJniObject album = QJniObject::fromString(state.album);
            const QJniObject target = QJniObject::fromString(state.targetName);
            m_bridge.callMethod<void>("update",
                "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JJZDI)V",
                title.object<jstring>(), artist.object<jstring>(), album.object<jstring>(), target.object<jstring>(),
                static_cast<jlong>(state.durationMs), static_cast<jlong>(state.positionMs),
                static_cast<jboolean>(state.playing), static_cast<jdouble>(state.playbackRate),
                static_cast<jint>(state.volume));
        }

        void clear() override
        {
            if (m_bridge.isValid())
                m_bridge.callMethod<void>("clear", "()V");
        }

    private:
        QJniObject m_bridge;
    };
#endif

} // namespace

std::unique_ptr<PlatformRemoteMediaSession> createPlatformRemoteMediaSession(QObject *parent)
{
#ifdef Q_OS_ANDROID
    if (platformCapabilities().isMobile)
        return std::make_unique<AndroidRemoteMediaSession>(parent);
#endif
    return std::make_unique<NullRemoteMediaSession>(parent);
}

#ifdef Q_OS_ANDROID
void dispatchAndroidRemoteMediaControl(int action, qint64 value)
{
    const QPointer<PlatformRemoteMediaSession> session = androidRemoteMediaSession;
    if (!session)
        return;
    QMetaObject::invokeMethod(
        session,
        [session, action, value]() {
            if (!session)
                return;
            switch (action) {
            case 0:
                emit session->playRequested();
                break;
            case 1:
                emit session->pauseRequested();
                break;
            case 2:
                emit session->playPauseRequested();
                break;
            case 3:
                emit session->stopRequested();
                break;
            case 4:
                emit session->nextRequested();
                break;
            case 5:
                emit session->previousRequested();
                break;
            case 6:
                emit session->seekRequested(value);
                break;
            case 7:
                emit session->seekRelativeRequested(value);
                break;
            case 8:
                emit session->volumeRequested(static_cast<int>(value));
                break;
            default:
                break;
            }
        },
        Qt::QueuedConnection);
}
#endif

} // namespace Spool

#ifdef Q_OS_ANDROID
extern "C" JNIEXPORT void JNICALL Java_com_sachk_spool_RemoteMediaSessionBridge_nativeControl(
    JNIEnv *, jclass, jint action, jlong value)
{
    Spool::dispatchAndroidRemoteMediaControl(static_cast<int>(action), static_cast<qint64>(value));
}
#endif
