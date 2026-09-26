#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

namespace Spool {

QStringList stringListFromVariant(const QVariant& value);
QStringList stringListFromVariantMap(const QVariantMap& map, const QString& key);

} // namespace Spool
