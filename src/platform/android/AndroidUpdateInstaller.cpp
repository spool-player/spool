#include "AndroidUpdateInstaller.h"

#include <QCoreApplication>
#include <QJniObject>

namespace Spool {

namespace {
    constexpr auto kJavaBridge = "com/sachk/spool/AndroidUpdateBridge";
}

bool AndroidUpdateInstaller::canRequestPackageInstalls()
{
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    return QJniObject::callStaticMethod<jboolean>(
        kJavaBridge, "canRequestPackageInstalls", "(Landroid/content/Context;)Z", context.object<jobject>());
}

bool AndroidUpdateInstaller::install(const QString& packagePath)
{
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const QJniObject path = QJniObject::fromString(packagePath);
    return QJniObject::callStaticMethod<jboolean>(kJavaBridge, "installApk",
        "(Landroid/content/Context;Ljava/lang/String;)Z", context.object<jobject>(), path.object<jstring>());
}

bool AndroidUpdateInstaller::openInstallSettings()
{
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    return QJniObject::callStaticMethod<jboolean>(
        kJavaBridge, "openInstallSettings", "(Landroid/content/Context;)Z", context.object<jobject>());
}

} // namespace Spool
