#pragma once

#include "../media/MediaTypes.h"

#include <QCoroTask>

#include <QString>

#include <functional>
#include <vector>

namespace Spool {

// Text search over a media source. Plain abstract class for the same reason
// as Catalog: the implementing facade is already a QObject through
// PlaybackSource.
class SearchSource {
public:
    virtual ~SearchSource() = default;

    virtual bool signedIn() const = 0;
    virtual QCoro::Task<std::vector<MovieItem>> searchItems(QString searchTerm, int limit = 80) = 0;
    virtual QCoro::Task<std::vector<MovieItem>> fetchSearchSuggestions(int limit = 20) = 0;

    // Hands `update` everything found so far each time more arrives, so one
    // slow source does not hold back the rest. A single source answers once.
    using SearchUpdate = std::function<void(std::vector<MovieItem>)>;
    virtual QCoro::Task<void> searchProgressively(QString searchTerm, int limit, SearchUpdate update)
    {
        update(co_await searchItems(std::move(searchTerm), limit));
    }
    // Search is about to be used; a chance to get connections ready.
    virtual void prepareSearch() { }
};

} // namespace Spool
