#pragma once

#include "../media/MediaTypes.h"

#include <QMap>
#include <QString>

#include <optional>
#include <vector>

class QJSValue;

namespace JellyfinNative {

// Experimental native-owned listing data. Keep the source with each item when
// aggregating pages; MovieItem::id alone is only meaningful within that source.
// This is not a stable SDK or a complete details/playback descriptor.
struct ProviderMediaItem {
    QString sourceId;
    MovieItem media;
    QMap<QString, QString> externalIds;
};

struct ProviderMediaPage {
    QString sourceId;
    std::vector<ProviderMediaItem> items;
    std::optional<QString> cursor;
    std::optional<qint64> total;
    bool exhausted = false;
};

namespace Detail {
// Invoke only on the owning script engine's thread. Reads the normalized
// provider schema directly, not Jellyfin JSON, and never retains JS values.
// The caller supplies trusted source identity and an explicit page-size bound.
ProviderMediaPage readProviderMediaPage(const QJSValue& value, const QString& sourceId, int maximumItems = 100);
}

} // namespace JellyfinNative
