#pragma once

#include <QString>

namespace JellyfinNative {

// The credentials a Jellyfin sign-in yields. Provider-owned: nothing in core
// reads a user id or server id.
struct AuthSession {
    QString userId;
    QString userName;
    QString accessToken;
    QString serverId;

    friend bool operator==(const AuthSession&, const AuthSession&) = default;
};

} // namespace JellyfinNative
