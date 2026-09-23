#pragma once

#include "provider/ArtworkSource.h"

#include <QString>
#include <QUrl>
#include <QUrlQuery>

namespace Spool::Testing {

// Renders each request as a query string so a test can read back what the
// artwork service asked for; the URL shape a real source produces is that
// source's own test.
class RecordingArtworkSource final : public ArtworkSource {
public:
    QString imageUrl(const ImageRequest& request) const override
    {
        if (request.itemId.isEmpty() || request.tag.isEmpty() || request.imageType.isEmpty())
            return {};
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("type"), request.imageType);
        if (request.fillWidth > 0 && request.fillHeight > 0) {
            query.addQueryItem(QStringLiteral("fillWidth"), QString::number(request.fillWidth));
            query.addQueryItem(QStringLiteral("fillHeight"), QString::number(request.fillHeight));
        } else {
            query.addQueryItem(QStringLiteral("maxWidth"), QString::number(request.maxWidth));
        }
        query.addQueryItem(QStringLiteral("quality"), QString::number(request.quality));
        query.addQueryItem(QStringLiteral("format"), request.format);
        query.addQueryItem(QStringLiteral("tag"), request.tag);
        QUrl url(QStringLiteral("https://example.test/") + request.itemId);
        url.setQuery(query);
        return url.toString(QUrl::FullyEncoded);
    }
};

} // namespace Spool::Testing
