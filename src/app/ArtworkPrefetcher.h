#pragma once

#include <QStringList>

namespace Spool {
struct MovieItem;

class ArtworkPrefetcher {
public:
    virtual ~ArtworkPrefetcher() = default;

    virtual void prefetch(const QStringList& urls) = 0;
    virtual void cancelPrefetches() = 0;
    virtual QString itemUrl(const MovieItem& item, bool landscape, int width = 0) const = 0;
};

} // namespace Spool
