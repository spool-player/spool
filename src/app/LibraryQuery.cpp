#include "LibraryQuery.h"
#include "../common/VariantUtils.h"

#include <QMetaType>
#include <QSet>
#include <QStringList>
#include <QVariant>

namespace Spool {

namespace {

    bool isSeriesLibrary(const LibraryItem& library)
    {
        return library.collectionType == QStringLiteral("tvshows");
    }

    bool libraryQueryHasValue(const QVariantMap& query, const QString& key)
    {
        const QVariant value = query.value(key);
        if (!value.isValid() || value.isNull())
            return false;
        if (value.typeId() == QMetaType::QString || value.typeId() == QMetaType::QByteArray)
            return !value.toString().isEmpty();
        if (value.typeId() == QMetaType::QStringList)
            return !value.toStringList().isEmpty();
        if (value.typeId() == QMetaType::QVariantList)
            return !value.toList().isEmpty();
        return value.toBool();
    }

    QString libraryQuerySignature(const QVariantMap& query, const LibraryItem& library)
    {
        const QVariantMap defaults = defaultLibraryQuery(library);
        if (activeLibraryFilterCount(query) == 0
            && query.value(QStringLiteral("sortBy"), defaults.value(QStringLiteral("sortBy"))).toString()
                == defaults.value(QStringLiteral("sortBy")).toString()
            && query.value(QStringLiteral("sortOrder"), defaults.value(QStringLiteral("sortOrder"))).toString()
                == defaults.value(QStringLiteral("sortOrder")).toString()) {
            return {};
        }

        QStringList parts;
        const QStringList keys = query.keys();
        for (const QString& key : keys) {
            const QVariant value = query.value(key);
            if (!libraryQueryHasValue(query, key))
                continue;
            if (value.typeId() == QMetaType::QStringList || value.typeId() == QMetaType::QVariantList) {
                QStringList values = libraryQueryStringList(query, key);
                values.sort();
                parts.push_back(QStringLiteral("%1=%2").arg(key, values.join(QLatin1Char(','))));
            } else {
                parts.push_back(QStringLiteral("%1=%2").arg(key, value.toString()));
            }
        }
        parts.sort();
        return parts.join(QLatin1Char('&'));
    }

} // namespace

QString libraryContentLabel(const LibraryItem& library)
{
    if (library.collectionType == QStringLiteral("tvshows"))
        return QStringLiteral("TV Shows");
    if (library.collectionType == QStringLiteral("movies"))
        return QStringLiteral("Movies");
    if (library.collectionType == QStringLiteral("musicvideos"))
        return QStringLiteral("Music Videos");
    if (library.collectionType == QStringLiteral("homevideos"))
        return QStringLiteral("Home Videos");
    return library.name.isEmpty() ? QStringLiteral("Library") : library.name;
}

QVariantMap defaultLibraryQuery(const LibraryItem& library)
{
    Q_UNUSED(library);
    return {
        { QStringLiteral("sortBy"), QStringLiteral("SortName") },
        { QStringLiteral("sortOrder"), QStringLiteral("Ascending") },
    };
}

bool supportsLatestLibraryRow(const LibraryItem& library)
{
    static const QSet<QString> excluded = {
        QStringLiteral("playlists"),
        QStringLiteral("livetv"),
        QStringLiteral("boxsets"),
        QStringLiteral("channels"),
        QStringLiteral("folders"),
    };
    return !library.id.isEmpty() && !excluded.contains(library.collectionType);
}

QStringList libraryQueryStringList(const QVariantMap& query, const QString& key)
{
    return stringListFromVariantMap(query, key);
}

int activeLibraryFilterCount(const QVariantMap& query)
{
    static const QStringList listKeys = {
        QStringLiteral("filters"),
        QStringLiteral("genres"),
        QStringLiteral("officialRatings"),
        QStringLiteral("tags"),
        QStringLiteral("years"),
        QStringLiteral("studioIds"),
        QStringLiteral("seriesStatus"),
        QStringLiteral("videoTypes"),
        QStringLiteral("includeItemTypes"),
    };
    static const QStringList valueKeys = {
        QStringLiteral("isHd"),
        QStringLiteral("is4K"),
        QStringLiteral("is3D"),
        QStringLiteral("hasSubtitles"),
        QStringLiteral("hasTrailer"),
        QStringLiteral("hasSpecialFeature"),
        QStringLiteral("hasThemeSong"),
        QStringLiteral("hasThemeVideo"),
        QStringLiteral("specialEpisode"),
        QStringLiteral("isMissing"),
        QStringLiteral("isUnaired"),
        QStringLiteral("alphabet"),
    };

    int count = 0;
    for (const QString& key : listKeys)
        count += libraryQueryStringList(query, key).size();
    for (const QString& key : valueKeys) {
        if (libraryQueryHasValue(query, key))
            ++count;
    }
    return count;
}

QString libraryCacheKey(const LibraryItem& library)
{
    if (isSeriesLibrary(library))
        return QStringLiteral("series/%1").arg(library.id);
    if (library.collectionType == QStringLiteral("movies"))
        return library.id;
    return QStringLiteral("library/%1/%2").arg(library.collectionType, library.id);
}

QString libraryCacheKey(const LibraryItem& library, const QVariantMap& query)
{
    const QString baseKey = libraryCacheKey(library);
    const QString signature = libraryQuerySignature(query, library);
    return signature.isEmpty() ? baseKey : QStringLiteral("%1?%2").arg(baseKey, signature);
}

} // namespace Spool
