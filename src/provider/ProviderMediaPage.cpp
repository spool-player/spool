#include "ProviderMediaPage.h"

#include <QJSValue>
#include <QJSValueIterator>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace JellyfinNative::Detail {
namespace {
    constexpr qsizetype kMaximumTextBytes = 4 * 1024 * 1024;
    constexpr double kMaximumSafeInteger = 9007199254740991.0;

    [[noreturn]] void invalid()
    {
        // Never put provider strings or response contents into public errors.
        throw std::runtime_error("invalid_media_page");
    }

    bool absent(const QJSValue& value)
    {
        return value.isNull() || value.isUndefined();
    }

    void requireObject(const QJSValue& value)
    {
        if (!value.isObject() || value.isArray() || value.isCallable() || value.isQObject() || value.isError())
            invalid();
    }

    quint32 arrayLength(const QJSValue& value, quint32 maximum)
    {
        if (!value.isArray())
            invalid();
        const quint32 length = value.property(QStringLiteral("length")).toUInt();
        if (length > maximum)
            invalid();
        return length;
    }

    // Exact ticks and byte counts can exceed JS's safe integers; providers send
    // those as decimal strings rather than rounded doubles.
    qint64 integer(const QJSValue& value)
    {
        if (absent(value))
            return 0;
        if (value.isNumber()) {
            const double number = value.toNumber();
            if (!std::isfinite(number) || number < 0 || number > kMaximumSafeInteger || std::trunc(number) != number)
                invalid();
            return static_cast<qint64>(number);
        }
        if (!value.isString())
            invalid();
        const QString decimal = value.toString();
        bool ok = decimal.size() <= 19 && !decimal.isEmpty()
            && std::all_of(decimal.begin(), decimal.end(), [](QChar c) { return c.isDigit(); });
        const qint64 result = ok ? decimal.toLongLong(&ok) : 0;
        if (!ok)
            invalid();
        return result;
    }

    int smallInteger(const QJSValue& value)
    {
        const qint64 result = integer(value);
        if (result > std::numeric_limits<int>::max())
            invalid();
        return static_cast<int>(result);
    }

    bool boolean(const QJSValue& value)
    {
        if (absent(value))
            return false;
        if (!value.isBool())
            invalid();
        return value.toBool();
    }

    double number(const QJSValue& value)
    {
        if (absent(value))
            return 0;
        if (!value.isNumber() || !std::isfinite(value.toNumber()))
            invalid();
        return value.toNumber();
    }

    struct Reader {
        qsizetype textBytes = 0;

        QString text(const QJSValue& value, qsizetype maximumLength = 64 * 1024)
        {
            if (absent(value))
                return {};
            if (!value.isString())
                invalid();
            QString result = value.toString();
            if (result.size() > maximumLength || (textBytes += result.size() * sizeof(QChar)) > kMaximumTextBytes)
                invalid();
            return result;
        }

        QString field(const QJSValue& row, const char *name, qsizetype maximumLength = 4096)
        {
            return text(row.property(QLatin1String(name)), maximumLength);
        }

        QStringList strings(const QJSValue& value)
        {
            if (absent(value))
                return {};
            QStringList result;
            const quint32 length = arrayLength(value, 256);
            result.reserve(length);
            for (quint32 i = 0; i < length; ++i)
                result.append(text(value.property(i), 4096));
            return result;
        }

        MediaStreamInfo stream(const QJSValue& row)
        {
            requireObject(row);
            MediaStreamInfo stream;
            stream.index = smallInteger(row.property(QStringLiteral("index")));
            stream.type = field(row, "type", 32);
            stream.codec = field(row, "codec", 64);
            stream.profile = field(row, "profile", 128);
            stream.language = field(row, "language", 64);
            stream.title = field(row, "title");
            stream.displayTitle = stream.title;
            stream.width = smallInteger(row.property(QStringLiteral("width")));
            stream.height = smallInteger(row.property(QStringLiteral("height")));
            stream.frameRate = number(row.property(QStringLiteral("frameRate")));
            stream.bitRate = smallInteger(row.property(QStringLiteral("bitrate")));
            stream.bitDepth = smallInteger(row.property(QStringLiteral("bitDepth")));
            stream.channels = smallInteger(row.property(QStringLiteral("channels")));
            stream.sampleRate = smallInteger(row.property(QStringLiteral("sampleRate")));
            stream.videoRange = field(row, "range", 32);
            stream.videoRangeType = field(row, "rangeType", 32);
            if (stream.videoRangeType.isEmpty())
                stream.videoRangeType = stream.videoRange;
            stream.isDefault = boolean(row.property(QStringLiteral("default")));
            stream.isForced = boolean(row.property(QStringLiteral("forced")));
            stream.isExternal = boolean(row.property(QStringLiteral("external")));
            stream.isInterlaced = boolean(row.property(QStringLiteral("interlaced")));
            return stream;
        }

        MediaSourceInfo variant(const QJSValue& row)
        {
            requireObject(row);
            MediaSourceInfo variant;
            variant.id = field(row, "id", 1024);
            if (variant.id.isEmpty())
                invalid();
            variant.name = field(row, "label");
            variant.path = field(row, "filename");
            variant.container = field(row, "container", 64);
            variant.size = integer(row.property(QStringLiteral("sizeBytes")));
            variant.bitRate = smallInteger(row.property(QStringLiteral("bitrate")));
            variant.runtimeTicks = integer(row.property(QStringLiteral("runtimeTicks")));
            const QJSValue streams = row.property(QStringLiteral("streams"));
            if (!absent(streams)) {
                const quint32 length = arrayLength(streams, 256);
                for (quint32 i = 0; i < length; ++i)
                    variant.streams.append(stream(streams.property(i)));
            }
            return variant;
        }

        MovieItem item(const QJSValue& row)
        {
            requireObject(row);
            MovieItem media;
            media.id = field(row, "id", 1024);
            if (media.id.isEmpty())
                invalid();
            media.title = field(row, "title");
            media.sortName = field(row, "sortName");
            if (media.sortName.isEmpty())
                media.sortName = media.title;
            media.itemType = field(row, "type", 128);
            media.overview = field(row, "overview", 64 * 1024);
            media.posterTag = field(row, "posterTag");
            media.backdropTag = field(row, "backdropTag");
            media.logoTag = field(row, "logoTag");
            media.bannerTag = field(row, "bannerTag");
            media.thumbTag = field(row, "thumbTag");
            media.seriesPrimaryImageTag = field(row, "seriesPosterTag");
            media.albumPrimaryImageTag = field(row, "albumPosterTag");
            media.seriesId = field(row, "seriesId", 1024);
            media.seasonId = field(row, "seasonId", 1024);
            media.seriesName = field(row, "seriesName");
            media.album = field(row, "album");
            media.albumId = field(row, "albumId", 1024);
            media.albumArtist = field(row, "albumArtist");
            media.year = smallInteger(row.property(QStringLiteral("year")));
            media.seasonNumber = smallInteger(row.property(QStringLiteral("season")));
            media.episodeNumber = smallInteger(row.property(QStringLiteral("episode")));
            media.runtimeTicks = integer(row.property(QStringLiteral("runtimeTicks")));
            media.resumeTicks = integer(row.property(QStringLiteral("resumeTicks")));
            media.playCount = smallInteger(row.property(QStringLiteral("playCount")));
            media.recursiveItemCount = smallInteger(row.property(QStringLiteral("childCount")));
            media.favorite = boolean(row.property(QStringLiteral("favorite")));
            media.played = boolean(row.property(QStringLiteral("played")));
            media.isVirtualItem = boolean(row.property(QStringLiteral("virtual")));
            media.locationType = media.isVirtualItem ? QStringLiteral("Virtual") : QString();
            media.dateCreated = field(row, "dateCreated", 64);
            media.datePlayed = field(row, "datePlayed", 64);
            media.dateLastContentAdded = field(row, "dateUpdated", 64);
            media.premiereDate = field(row, "premiereDate", 64);
            media.endDate = field(row, "endDate", 64);
            media.status = field(row, "status", 64);
            media.genres = strings(row.property(QStringLiteral("genres")));
            media.tags = strings(row.property(QStringLiteral("tags")));
            media.studios = strings(row.property(QStringLiteral("studios")));
            media.officialRating = field(row, "officialRating", 1024);
            media.communityRating = number(row.property(QStringLiteral("communityRating")));
            media.criticRating = number(row.property(QStringLiteral("criticRating")));

            const QJSValue ids = row.property(QStringLiteral("externalIds"));
            if (!absent(ids)) {
                requireObject(ids);
                QJSValueIterator iterator(ids);
                for (int count = 0; iterator.hasNext(); ++count) {
                    iterator.next();
                    if (count >= 64)
                        invalid();
                    const QString key = iterator.name().trimmed().toLower();
                    const QString id = text(iterator.value(), 1024);
                    if (key == QStringLiteral("imdb"))
                        media.imdbId = id;
                    else if (key == QStringLiteral("tmdb"))
                        media.tmdbId = id;
                }
            }
            const QJSValue links = row.property(QStringLiteral("links"));
            if (!absent(links)) {
                const quint32 length = arrayLength(links, 32);
                for (quint32 i = 0; i < length; ++i) {
                    const QJSValue link = links.property(i);
                    requireObject(link);
                    media.externalUrls.append({ field(link, "name"), field(link, "url") });
                }
            }
            const QJSValue people = row.property(QStringLiteral("people"));
            if (!absent(people)) {
                const quint32 length = arrayLength(people, 512);
                media.people.reserve(length);
                for (quint32 i = 0; i < length; ++i) {
                    const QJSValue person = people.property(i);
                    requireObject(person);
                    media.people.append({ field(person, "id", 1024), field(person, "name"), field(person, "type", 128),
                        field(person, "role"), field(person, "imageTag") });
                }
            }
            const QJSValue variants = row.property(QStringLiteral("variants"));
            if (!absent(variants)) {
                const quint32 length = arrayLength(variants, 64);
                for (quint32 i = 0; i < length; ++i)
                    media.mediaSources.append(variant(variants.property(i)));
            }
            return media;
        }
    };
} // namespace

ProviderMediaPage readProviderMediaPage(const QJSValue& value, int maximumItems)
{
    requireObject(value);
    const QJSValue rows = value.property(QStringLiteral("items"));
    const QJSValue exhausted = value.property(QStringLiteral("exhausted"));
    if (!exhausted.isBool())
        invalid();
    const quint32 length = arrayLength(rows, static_cast<quint32>(std::clamp(maximumItems, 1, 1000)));
    Reader reader;
    ProviderMediaPage result;
    result.exhausted = exhausted.toBool();
    const QJSValue cursor = value.property(QStringLiteral("cursor"));
    if (!absent(cursor)) {
        result.cursor = reader.text(cursor, 4096);
        if (result.cursor->isEmpty())
            invalid();
    }
    if (!result.exhausted && !result.cursor)
        invalid();
    const QJSValue total = value.property(QStringLiteral("total"));
    if (!absent(total))
        result.total = integer(total);
    result.items.reserve(length);
    for (quint32 i = 0; i < length; ++i)
        result.items.push_back(reader.item(rows.property(i)));
    return result;
}

MovieItem readProviderItem(const QJSValue& value)
{
    return Reader().item(value);
}

} // namespace JellyfinNative::Detail
