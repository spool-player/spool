#pragma once

#include <QStringList>

namespace Spool {
struct MovieItem;

class ArtworkPrefetcher {
public:
    virtual ~ArtworkPrefetcher() = default;

    // `retarget` drops whatever is still queued first: the view has moved on.
    virtual void prefetch(const QStringList& urls, bool retarget = false) = 0;
    virtual void cancelPrefetches() = 0;
    virtual QString itemUrl(const MovieItem& item, bool landscape, int width = 0) const = 0;
};

} // namespace Spool
