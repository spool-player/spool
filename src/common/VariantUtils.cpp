#include "VariantUtils.h"

#include <QMetaType>

namespace Spool {

QStringList stringListFromVariant(const QVariant& value)
{
    if (!value.isValid() || value.isNull())
        return {};

    if (value.typeId() == QMetaType::QStringList)
        return value.toStringList();

    QStringList result;
    if (value.typeId() == QMetaType::QVariantList) {
        const QVariantList list = value.toList();
        result.reserve(list.size());
        for (const QVariant& item : list) {
            const QString text = item.toString();
            if (!text.isEmpty())
                result.push_back(text);
        }
    } else {
        const QString text = value.toString();
        if (!text.isEmpty())
            result = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    }

    result.removeAll(QString());
    return result;
}

QStringList stringListFromVariantMap(const QVariantMap& map, const QString& key)
{
    return stringListFromVariant(map.value(key));
}

} // namespace Spool
