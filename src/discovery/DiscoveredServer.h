#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>

namespace JellyfinNative {

struct DiscoveredServer {
    Q_GADGET
    Q_PROPERTY(QString id MEMBER id)
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString address MEMBER address)

public:
    QString id;
    QString name;
    QString address;
};

} // namespace JellyfinNative

Q_DECLARE_METATYPE(JellyfinNative::DiscoveredServer)
