#pragma once

#include "../media/MediaTypes.h"

#include <QCoroTask>

#include <QString>

#include <vector>

namespace JellyfinNative {

// Text search over a media source. Plain abstract class for the same reason
// as Catalog: the implementing facade is already a QObject through
// PlaybackSource.
class SearchSource {
public:
    virtual ~SearchSource() = default;

    virtual bool signedIn() const = 0;
    virtual QCoro::Task<std::vector<MovieItem>> searchItems(QString searchTerm, int limit = 80) = 0;
    virtual QCoro::Task<std::vector<MovieItem>> fetchSearchSuggestions(int limit = 20) = 0;
};

} // namespace JellyfinNative
