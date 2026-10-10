#include "platform/CredentialStore.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

namespace Spool::CredentialStore {
namespace {
    NSMutableDictionary *query(const QString& profileId = {})
    {
        auto *result = [@{ (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
            (__bridge id)kSecAttrService: @SPOOL_APPLE_CREDENTIAL_SERVICE } mutableCopy];
        if (!profileId.isEmpty())
            result[(__bridge id)kSecAttrAccount] = [NSString stringWithUTF8String:profileId.toUtf8().constData()];
        return result;
    }
}
QString load(const QString& profileId)
{
    if (profileId.isEmpty())
        return {};
    auto *request = query(profileId);
    request[(__bridge id)kSecReturnData] = @YES;
    request[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    CFTypeRef value = nullptr;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)request, &value) != errSecSuccess)
        return {};
    NSData *data = CFBridgingRelease(value);
    return QString::fromUtf8(static_cast<const char *>(data.bytes), static_cast<qsizetype>(data.length));
}
bool save(const QString& profileId, const QString& accessToken)
{
    if (profileId.isEmpty())
        return false;
    const QByteArray token = accessToken.toUtf8();
    NSData *data = [NSData dataWithBytes:token.constData() length:token.size()];
    auto *request = query(profileId);
    NSDictionary *attributes = @{ (__bridge id)kSecValueData: data,
        (__bridge id)kSecAttrAccessible: (__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly };
    OSStatus status = SecItemUpdate((__bridge CFDictionaryRef)request, (__bridge CFDictionaryRef)attributes);
    if (status == errSecItemNotFound) {
        [request addEntriesFromDictionary:attributes];
        status = SecItemAdd((__bridge CFDictionaryRef)request, nullptr);
    }
    return status == errSecSuccess;
}
void remove(const QString& profileId)
{
    if (!profileId.isEmpty())
        SecItemDelete((__bridge CFDictionaryRef)query(profileId));
}
void clear()
{
    SecItemDelete((__bridge CFDictionaryRef)query());
}
} // namespace Spool::CredentialStore
