#include "ProviderMediaPage.h"

#include <QJSValue>
#include <QJSValueIterator>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace JellyfinNative::Detail {
namespace {
    constexpr qsizetype maximumTextBytes = 4 * 1024 * 1024;
    constexpr double maximumSafeInteger = 9007199254740991.0;

    [[noreturn]] void invalid()
    {
        // Never put provider strings or response contents into public errors.
        throw std::runtime_error("invalid_media_page");
    }

    bool absent(const QJSValue& value)
    {
        return value.isNull() || value.isUndefined();
    }

    void object(const QJSValue& value)
    {
        if (!value.isObject() || value.isArray() || value.isCallable() || value.isQObject() || value.isError())
            invalid();
    }

    qint64 integer(const QJSValue& value, qint64 fallback = 0)
    {
        if (absent(value))
            return fallback;
        if (value.isNumber()) {
            const double number = value.toNumber();
            if (!std::isfinite(number) || number < 0 || number > maximumSafeInteger || std::trunc(number) != number)
                invalid();
            return static_cast<qint64>(number);
        }
        // Exact ticks/byte counts can exceed JS's safe integer range. Providers
        // must transmit those as decimal strings, never as rounded doubles.
        if (!value.isString())
            invalid();
        const QString decimal = value.toString();
        if (decimal.isEmpty() || decimal.size() > 19)
            invalid();
        for (const QChar digit : decimal) {
            if (digit < QLatin1Char('0') || digit > QLatin1Char('9'))
                invalid();
        }
        bool ok = false;
        const qint64 result = decimal.toLongLong(&ok);
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
            if (result.size() > maximumLength)
                invalid();
            textBytes += result.size() * sizeof(QChar);
            if (textBytes > maximumTextBytes)
                invalid();
            return result;
        }

        QStringList strings(const QJSValue& value)
        {
            if (absent(value))
                return {};
            if (!value.isArray())
                invalid();
            const quint32 length = value.property(QStringLiteral("length")).toUInt();
            if (length > 256)
                invalid();
            QStringList result;
            result.reserve(length);
            for (quint32 i = 0; i < length; ++i)
                result.append(text(value.property(i), 4096));
            return result;
        }

        ProviderMediaItem item(const QJSValue& row, const QString& sourceId)
        {
            object(row);
            ProviderMediaItem result;
            result.sourceId = sourceId; // Ignore any source identity in provider data.
            MovieItem& media = result.media;
            media.id = text(row.property(QStringLiteral("id")), 1024);
            if (media.id.isEmpty())
                invalid();
            media.title = text(row.property(QStringLiteral("title")));
            media.sortName = text(row.property(QStringLiteral("sortName")));
            if (media.sortName.isEmpty())
                media.sortName = media.title;
            media.itemType = text(row.property(QStringLiteral("type")), 128);
            media.overview = text(row.property(QStringLiteral("overview")));
            media.posterTag = text(row.property(QStringLiteral("posterTag")), 4096);
            media.backdropTag = text(row.property(QStringLiteral("backdropTag")), 4096);
            media.logoTag = text(row.property(QStringLiteral("logoTag")), 4096);
            media.seriesId = text(row.property(QStringLiteral("seriesId")), 1024);
            media.seasonId = text(row.property(QStringLiteral("seasonId")), 1024);
            media.seriesName = text(row.property(QStringLiteral("seriesName")));
            media.year = smallInteger(row.property(QStringLiteral("year")));
            media.seasonNumber = smallInteger(row.property(QStringLiteral("season")));
            media.episodeNumber = smallInteger(row.property(QStringLiteral("episode")));
            media.runtimeTicks = integer(row.property(QStringLiteral("runtimeTicks")));
            media.resumeTicks = integer(row.property(QStringLiteral("resumeTicks")));
            media.favorite = boolean(row.property(QStringLiteral("favorite")));
            media.played = boolean(row.property(QStringLiteral("played")));
            media.genres = strings(row.property(QStringLiteral("genres")));
            media.tags = strings(row.property(QStringLiteral("tags")));
            media.studios = strings(row.property(QStringLiteral("studios")));
            media.officialRating = text(row.property(QStringLiteral("officialRating")), 1024);
            media.communityRating = number(row.property(QStringLiteral("communityRating")));

            const QJSValue ids = row.property(QStringLiteral("externalIds"));
            if (!absent(ids)) {
                object(ids);
                QJSValueIterator iterator(ids);
                int count = 0;
                while (iterator.hasNext()) {
                    iterator.next();
                    if (++count > 64 || iterator.name().isEmpty() || iterator.name().size() > 128)
                        invalid();
                    const QString key = iterator.name().trimmed().toLower();
                    const QString id = text(iterator.value(), 1024);
                    if (key.isEmpty())
                        invalid();
                    if (id.isEmpty())
                        continue;
                    if (result.externalIds.contains(key) && result.externalIds.value(key) != id)
                        invalid();
                    result.externalIds.insert(key, id);
                }
            }
            media.imdbId = result.externalIds.value(QStringLiteral("imdb"));
            media.tmdbId = result.externalIds.value(QStringLiteral("tmdb"));

            const QJSValue people = row.property(QStringLiteral("people"));
            if (!absent(people)) {
                if (!people.isArray())
                    invalid();
                const quint32 length = people.property(QStringLiteral("length")).toUInt();
                if (length > 512)
                    invalid();
                media.people.reserve(length);
                for (quint32 i = 0; i < length; ++i) {
                    const QJSValue person = people.property(i);
                    object(person);
                    PersonItem entry;
                    entry.id = text(person.property(QStringLiteral("id")), 1024);
                    entry.name = text(person.property(QStringLiteral("name")));
                    entry.type = text(person.property(QStringLiteral("type")), 128);
                    entry.role = text(person.property(QStringLiteral("role")));
                    entry.imageTag = text(person.property(QStringLiteral("imageTag")), 4096);
                    media.people.append(std::move(entry));
                }
            }
            return result;
        }
    };
} // namespace

ProviderMediaPage readProviderMediaPage(const QJSValue& value, const QString& sourceId, int maximumItems)
{
    if (sourceId.isEmpty() || sourceId.size() > 256 || maximumItems < 1 || maximumItems > 1000)
        invalid();
    object(value);
    const QJSValue rows = value.property(QStringLiteral("items"));
    const QJSValue exhausted = value.property(QStringLiteral("exhausted"));
    if (!rows.isArray() || !exhausted.isBool())
        invalid();
    const quint32 length = rows.property(QStringLiteral("length")).toUInt();
    if (length > static_cast<quint32>(maximumItems))
        invalid();
    Reader reader;
    ProviderMediaPage result;
    result.sourceId = sourceId;
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
        result.items.push_back(reader.item(rows.property(i), sourceId));
    return result;
}
} // namespace JellyfinNative::Detail
