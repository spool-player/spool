#pragma once

#include <QHostAddress>
#include <QString>

namespace Spool {

inline bool isPrivateNetworkAddress(const QHostAddress& address)
{
    if (address.isLoopback())
        return true;
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR bytes = address.toIPv6Address();
        return (bytes[0] & 0xfeu) == 0xfcu || (bytes[0] == 0xfeu && (bytes[1] & 0xc0u) == 0x80u);
    }
    if (address.protocol() != QAbstractSocket::IPv4Protocol)
        return false;

    const quint32 value = address.toIPv4Address();
    return (value & 0xff000000u) == 0x0a000000u || (value & 0xfff00000u) == 0xac100000u
        || (value & 0xffff0000u) == 0xc0a80000u || (value & 0xffc00000u) == 0x64400000u
        || (value & 0xffff0000u) == 0xa9fe0000u;
}

inline bool isLanHost(const QString& host)
{
    QHostAddress address;
    if (address.setAddress(host))
        return isPrivateNetworkAddress(address);
    const QString normalized = host.trimmed().toLower();
    return normalized == QStringLiteral("localhost") || normalized.endsWith(QStringLiteral(".local"))
        || !normalized.contains(QLatin1Char('.'));
}

} // namespace Spool
