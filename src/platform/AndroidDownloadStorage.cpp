#include "AndroidDownloadStorage.h"

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>
#include <QMetaObject>
#include <QMimeDatabase>
#include <QPointer>
#include <QtCore/private/qjnihelpers_p.h>
#include <QtCore/qnativeinterface.h>
#endif

namespace Spool {

#ifdef Q_OS_ANDROID
namespace {
    constexpr jint folderRequestCode = 0x5350;
    constexpr jint readWriteGrant = 0x00000003;
    constexpr jint persistableGrant = 0x00000040;
    constexpr jint prefixGrant = 0x00000080;
    constexpr auto documentsClass = "android/provider/DocumentsContract";

    QJniObject androidUri(const QUrl& url)
    {
        if (!url.isValid() || url.scheme() != QLatin1String("content"))
            return {};
        const QJniObject text = QJniObject::fromString(url.toString(QUrl::FullyEncoded));
        return QJniObject::callStaticObjectMethod(
            "android/net/Uri", "parse", "(Ljava/lang/String;)Landroid/net/Uri;", text.object<jstring>());
    }

    QUrl documentUrl(const QJniObject& uri)
    {
        return uri.isValid() ? QUrl(uri.toString()) : QUrl {};
    }

    QJniObject contentResolver()
    {
        const QJniObject context = QNativeInterface::QAndroidApplication::context();
        return context.isValid() ? context.callObjectMethod("getContentResolver", "()Landroid/content/ContentResolver;")
                                 : QJniObject {};
    }

    // QJniObject's void calls clear Java exceptions. Use JNI here so a refused
    // persisted grant or failed launch is reported instead of treated as success.
    bool persistGrant(const QJniObject& resolver, const QJniObject& uri, jint flags)
    {
        QJniEnvironment env;
        const jmethodID method
            = env->GetMethodID(resolver.objectClass(), "takePersistableUriPermission", "(Landroid/net/Uri;I)V");
        if (env.checkAndClearExceptions() || !method)
            return false;
        env->CallVoidMethod(resolver.object<jobject>(), method, uri.object<jobject>(), flags);
        return !env.checkAndClearExceptions();
    }
}

class AndroidDownloadStorage::ResultReceiver final : public QtAndroidPrivate::ActivityResultListener {
public:
    explicit ResultReceiver(AndroidDownloadStorage *owner)
        : m_owner(owner)
    {
        QtAndroidPrivate::registerActivityResultListener(this);
    }

    ~ResultReceiver() override
    {
        QtAndroidPrivate::unregisterActivityResultListener(this);
    }

    bool handleActivityResult(jint requestCode, jint resultCode, jobject data) override
    {
        if (requestCode != folderRequestCode)
            return false;
        const QJniObject intent(data);
        QMetaObject::invokeMethod(
            m_owner,
            [owner = m_owner, resultCode, intent]() {
                owner->m_pickerOpen = false;
                if (resultCode == 0) // Activity.RESULT_CANCELED
                    return;
                if (resultCode != -1 || !intent.isValid()) { // Activity.RESULT_OK
                    emit owner->problem(QStringLiteral("The download folder could not be selected."));
                    return;
                }
                const QJniObject uri = intent.callObjectMethod("getData", "()Landroid/net/Uri;");
                const QUrl tree = documentUrl(uri);
                const jint grants = intent.callMethod<jint>("getFlags", "()I") & readWriteGrant;
                const QJniObject resolver = contentResolver();
                if (!AndroidDownloadStorage::isTree(tree) || grants != readWriteGrant || !resolver.isValid()
                    || !persistGrant(resolver, uri, grants)) {
                    emit owner->problem(
                        QStringLiteral("The selected folder did not provide persistent read and write access."));
                    return;
                }
                emit owner->folderSelected(tree);
            },
            Qt::QueuedConnection);
        return true;
    }

private:
    AndroidDownloadStorage *m_owner;
};
#endif

AndroidDownloadStorage::AndroidDownloadStorage(QObject *parent)
    : QObject(parent)
{
#ifdef Q_OS_ANDROID
    m_receiver = std::make_unique<ResultReceiver>(this);
#endif
}

AndroidDownloadStorage::~AndroidDownloadStorage() = default;

void AndroidDownloadStorage::chooseFolder()
{
#ifdef Q_OS_ANDROID
    if (m_pickerOpen)
        return;
    m_pickerOpen = true;
    QPointer<AndroidDownloadStorage> owner(this);
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([owner]() {
        if (!owner)
            return;
        const QJniObject activity = QtAndroidPrivate::activity();
        const QJniObject action = QJniObject::fromString(QStringLiteral("android.intent.action.OPEN_DOCUMENT_TREE"));
        const QJniObject intent("android/content/Intent", "(Ljava/lang/String;)V", action.object<jstring>());
        if (intent.isValid())
            intent.callObjectMethod(
                "addFlags", "(I)Landroid/content/Intent;", readWriteGrant | persistableGrant | prefixGrant);
        bool launched = false;
        if (activity.isValid() && intent.isValid()) {
            QJniEnvironment env;
            const jmethodID method
                = env->GetMethodID(activity.objectClass(), "startActivityForResult", "(Landroid/content/Intent;I)V");
            if (!env.checkAndClearExceptions() && method) {
                env->CallVoidMethod(activity.object<jobject>(), method, intent.object<jobject>(), folderRequestCode);
                launched = !env.checkAndClearExceptions();
            }
        }
        if (!launched && owner) {
            QMetaObject::invokeMethod(
                owner.data(),
                [owner]() {
                    if (!owner)
                        return;
                    owner->m_pickerOpen = false;
                    emit owner->problem(QStringLiteral("The system download folder picker is unavailable."));
                },
                Qt::QueuedConnection);
        }
    });
#else
    emit problem(QStringLiteral("The Android download folder picker is not supported on this platform."));
#endif
}

bool AndroidDownloadStorage::isTree(const QUrl& url)
{
#ifdef Q_OS_ANDROID
    const QJniObject uri = androidUri(url);
    return uri.isValid()
        && QJniObject::callStaticMethod<jboolean>(
            documentsClass, "isTreeUri", "(Landroid/net/Uri;)Z", uri.object<jobject>());
#else
    Q_UNUSED(url)
    return false;
#endif
}

QUrl AndroidDownloadStorage::createFile(const QUrl& tree, const QString& name)
{
#ifdef Q_OS_ANDROID
    if (!isTree(tree) || name.isEmpty())
        return {};
    const QJniObject resolver = contentResolver();
    if (!resolver.isValid())
        return {};
    const QJniObject uri = androidUri(tree);
    const QJniObject id = QJniObject::callStaticObjectMethod(
        documentsClass, "getTreeDocumentId", "(Landroid/net/Uri;)Ljava/lang/String;", uri.object<jobject>());
    if (!id.isValid())
        return {};
    const QJniObject parent = QJniObject::callStaticObjectMethod(documentsClass, "buildDocumentUriUsingTree",
        "(Landroid/net/Uri;Ljava/lang/String;)Landroid/net/Uri;", uri.object<jobject>(), id.object<jstring>());
    if (!parent.isValid())
        return {};
    const QJniObject mime
        = QJniObject::fromString(QMimeDatabase().mimeTypeForFile(name, QMimeDatabase::MatchExtension).name());
    const QJniObject displayName = QJniObject::fromString(name);
    return documentUrl(QJniObject::callStaticObjectMethod(documentsClass, "createDocument",
        "(Landroid/content/ContentResolver;Landroid/net/Uri;Ljava/lang/String;Ljava/lang/String;)Landroid/net/Uri;",
        resolver.object<jobject>(), parent.object<jobject>(), mime.object<jstring>(), displayName.object<jstring>()));
#else
    Q_UNUSED(tree)
    Q_UNUSED(name)
    return {};
#endif
}

QUrl AndroidDownloadStorage::renameFile(const QUrl& document, const QString& name)
{
#ifdef Q_OS_ANDROID
    const QJniObject uri = androidUri(document);
    const QJniObject resolver = contentResolver();
    if (!uri.isValid() || !resolver.isValid() || name.isEmpty())
        return {};
    const QJniObject displayName = QJniObject::fromString(name);
    return documentUrl(QJniObject::callStaticObjectMethod(documentsClass, "renameDocument",
        "(Landroid/content/ContentResolver;Landroid/net/Uri;Ljava/lang/String;)Landroid/net/Uri;",
        resolver.object<jobject>(), uri.object<jobject>(), displayName.object<jstring>()));
#else
    Q_UNUSED(document)
    Q_UNUSED(name)
    return {};
#endif
}

bool AndroidDownloadStorage::removeFile(const QUrl& document)
{
#ifdef Q_OS_ANDROID
    const QJniObject uri = androidUri(document);
    const QJniObject resolver = contentResolver();
    return uri.isValid() && resolver.isValid()
        && QJniObject::callStaticMethod<jboolean>(documentsClass, "deleteDocument",
            "(Landroid/content/ContentResolver;Landroid/net/Uri;)Z", resolver.object<jobject>(), uri.object<jobject>());
#else
    Q_UNUSED(document)
    return false;
#endif
}

} // namespace Spool
