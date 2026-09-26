#include "../CredentialStore.h"
#include "../common/CredentialStoreFileBackend.h"

#include <QByteArray>
#include <QProcess>

#include <Security/Security.h>

namespace Spool::CredentialStore {
namespace {
    constexpr char kService[] = SPOOL_MACOS_CREDENTIAL_SERVICE;
}

QString load(const QString& profileId)
{
    if (FileBackend::enabled())
        return FileBackend::load(profileId);
    const QByteArray account = profileId.toUtf8();
    void *data = nullptr;
    UInt32 length = 0;
    SecKeychainItemRef item = nullptr;
    const OSStatus status = SecKeychainFindGenericPassword(nullptr, sizeof(kService) - 1, kService,
        static_cast<UInt32>(account.size()), account.constData(), &length, &data, &item);
    if (status != errSecSuccess)
        return {};
    const QString token = QString::fromUtf8(static_cast<const char *>(data), static_cast<qsizetype>(length));
    SecKeychainItemFreeContent(nullptr, data);
    if (item)
        CFRelease(item);
    return token;
}

bool save(const QString& profileId, const QString& accessToken)
{
    if (FileBackend::enabled())
        return FileBackend::save(profileId, accessToken);
    const QByteArray account = profileId.toUtf8();
    const QByteArray token = accessToken.toUtf8();
    SecKeychainItemRef item = nullptr;
    const OSStatus found = SecKeychainFindGenericPassword(nullptr, sizeof(kService) - 1, kService,
        static_cast<UInt32>(account.size()), account.constData(), nullptr, nullptr, &item);
    OSStatus status = errSecSuccess;
    if (found == errSecSuccess) {
        status = SecKeychainItemModifyAttributesAndData(
            item, nullptr, static_cast<UInt32>(token.size()), token.constData());
    } else if (found == errSecItemNotFound) {
        status = SecKeychainAddGenericPassword(nullptr, sizeof(kService) - 1, kService,
            static_cast<UInt32>(account.size()), account.constData(), static_cast<UInt32>(token.size()),
            token.constData(), nullptr);
    } else {
        status = found;
    }
    if (item)
        CFRelease(item);
    return status == errSecSuccess;
}

void remove(const QString& profileId)
{
    if (FileBackend::enabled()) {
        FileBackend::remove(profileId);
        return;
    }
    const QByteArray account = profileId.toUtf8();
    SecKeychainItemRef item = nullptr;
    if (SecKeychainFindGenericPassword(nullptr, sizeof(kService) - 1, kService, static_cast<UInt32>(account.size()),
            account.constData(), nullptr, nullptr, &item)
        == errSecSuccess) {
        SecKeychainItemDelete(item);
    }
    if (item)
        CFRelease(item);
}

void clear()
{
    if (FileBackend::enabled()) {
        FileBackend::clear();
        return;
    }
    QProcess::execute(QStringLiteral("/usr/bin/security"),
        { QStringLiteral("delete-generic-password"), QStringLiteral("-s"), QString::fromLatin1(kService) });
}

} // namespace Spool::CredentialStore
