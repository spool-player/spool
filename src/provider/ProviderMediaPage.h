#pragma once

#include "../media/MediaTypes.h"

#include <QString>

#include <optional>
#include <vector>

class QJSValue;

namespace Spool {

// One page of a provider listing, decoded on the worker. IDs are the
// provider's own; the hub scopes them to their source.
struct ProviderMediaPage {
    std::vector<MovieItem> items;
    std::optional<QString> cursor;
    std::optional<qint64> total;
    bool exhausted = false;
};

namespace Detail {
    // Worker-thread only: reads the normalized provider schema (not any
    // backend's JSON) straight into native values and keeps no JS values.
    ProviderMediaPage readProviderMediaPage(const QJSValue& value, int maximumItems = 100);
    MovieItem readProviderItem(const QJSValue& value);
}

} // namespace Spool
