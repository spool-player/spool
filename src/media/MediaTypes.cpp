#include "MediaTypes.h"
#include "../common/MetaJson.h"
#include "../platform/PlatformSettingsPolicy.h"

#include <QDate>
#include <QJsonValue>
#include <QLocale>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>
#include <stdexcept>

namespace Spool {

namespace {

    constexpr qint64 kTicksPerSecond = 10000000;

    bool streamLanguagesMatch(const QString& requested, const QString& available)
    {
        const QString a = requested.trimmed().toLower();
        const QString b = available.trimmed().toLower();
        if (a.isEmpty() || b.isEmpty())
            return false;
        return a == b || (a.size() >= 2 && b.size() >= 2 && (a.startsWith(b) || b.startsWith(a)));
    }

    QString languageTag(const QString& language)
    {
        const QString trimmed = language.trimmed().toUpper();
        return trimmed.isEmpty() ? QStringLiteral("UND") : trimmed;
    }

    QString channelLayoutLabel(int channels)
    {
        switch (channels) {
        case 1:
            return QStringLiteral("1.0");
        case 2:
            return QStringLiteral("2.0");
        case 3:
            return QStringLiteral("2.1");
        case 5:
            return QStringLiteral("4.1");
        case 6:
            return QStringLiteral("5.1");
        case 7:
            return QStringLiteral("6.1");
        case 8:
            return QStringLiteral("7.1");
        default:
            return channels > 0 ? QStringLiteral("%1ch").arg(channels) : QString();
        }
    }

    QString resolutionLabel(int width, int height)
    {
        if (width <= 0 && height <= 0)
            return {};
        if (width >= 6000 || height >= 3100)
            return QStringLiteral("8K UHD");
        if (width >= 3200 || height >= 1660)
            return QStringLiteral("4K UHD");
        if (width >= 1900 || height >= 1000)
            return QStringLiteral("1080p HD");
        if (width >= 1260 || height >= 700)
            return QStringLiteral("720p HD");
        return QStringLiteral("SD");
    }

    QString hdrTypeLabel(const QString& value)
    {
        const QString type = value.trimmed().toUpper();
        if (type == QStringLiteral("HDR10"))
            return QStringLiteral("HDR10");
        if (type == QStringLiteral("HDR10PLUS") || type == QStringLiteral("HDR10+"))
            return QStringLiteral("HDR10+");
        if (type == QStringLiteral("HLG"))
            return QStringLiteral("HLG");
        if (type == QStringLiteral("DOVIWITHHDR10PLUS") || type == QStringLiteral("DOVIWITHELHDR10PLUS"))
            return QStringLiteral("Dolby Vision / HDR10+");
        if (type == QStringLiteral("DOVI") || type == QStringLiteral("DOLBY VISION")
            || type == QStringLiteral("DOVIWITHHDR10") || type == QStringLiteral("DOVIWITHHLG")
            || type == QStringLiteral("DOVIWITHSDR") || type == QStringLiteral("DOVIWITHEL"))
            return QStringLiteral("Dolby Vision");
        return {};
    }

    QString hdrFormatLabel(const MediaStreamInfo& stream)
    {
        const QString format = hdrTypeLabel(stream.videoRangeType);
        if (!format.isEmpty())
            return format;
        if (stream.videoRangeType.trimmed().compare(QStringLiteral("SDR"), Qt::CaseInsensitive) == 0)
            return {};
        const QString rangeFormat = hdrTypeLabel(stream.videoRange);
        if (!rangeFormat.isEmpty())
            return rangeFormat;
        if (stream.videoRange.trimmed().compare(QStringLiteral("SDR"), Qt::CaseInsensitive) == 0)
            return {};

        // Match the PQ/HLG transfer identifiers used by playback HDR detection.
        // PQ alone establishes HDR, not HDR10 mastering or dynamic metadata.
        const QString transfer = stream.colorTransfer.trimmed().toLower();
        if (transfer == QStringLiteral("hlg") || transfer.contains(QStringLiteral("b67")))
            return QStringLiteral("HLG");
        if (transfer == QStringLiteral("pq") || transfer.contains(QStringLiteral("2084"))
            || stream.videoRange.trimmed().compare(QStringLiteral("HDR"), Qt::CaseInsensitive) == 0)
            return QStringLiteral("HDR");
        return {};
    }

    QString runtimeLabel(qint64 runtimeTicks)
    {
        const qint64 totalSeconds = runtimeTicks / kTicksPerSecond;
        if (totalSeconds <= 0)
            return {};
        const qint64 hours = totalSeconds / 3600;
        const qint64 minutes = (totalSeconds % 3600) / 60;
        const qint64 seconds = totalSeconds % 60;
        if (hours > 0)
            return QStringLiteral("%1:%2:%3")
                .arg(hours)
                .arg(minutes, 2, 10, QLatin1Char('0'))
                .arg(seconds, 2, 10, QLatin1Char('0'));
        return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
    }

    QString releaseDateLabel(const QString& premiereDate)
    {
        const QDate date = QDate::fromString(premiereDate.left(10), Qt::ISODate);
        return date.isValid() ? QLocale().toString(date, QStringLiteral("d MMM yyyy")) : QString();
    }

    QString browseKindKey(BrowseKind kind)
    {
        switch (kind) {
        case BrowseKind::Library:
            return QStringLiteral("library");
        case BrowseKind::FolderChildren:
            return QStringLiteral("folderChildren");
        case BrowseKind::Person:
            return QStringLiteral("person");
        case BrowseKind::Genre:
            return QStringLiteral("genre");
        case BrowseKind::Studio:
            return QStringLiteral("studio");
        case BrowseKind::SeriesSeasons:
            return QStringLiteral("seriesSeasons");
        case BrowseKind::SeasonEpisodes:
            return QStringLiteral("seasonEpisodes");
        case BrowseKind::Playlist:
            return QStringLiteral("playlist");
        case BrowseKind::BoxSet:
            return QStringLiteral("boxset");
        case BrowseKind::ArtistAlbums:
            return QStringLiteral("artistAlbums");
        case BrowseKind::None:
            break;
        }
        return QStringLiteral("none");
    }

    QString queryValueSignature(const QVariant& value)
    {
        if (value.typeId() == QMetaType::QStringList) {
            QStringList items = value.toStringList();
            items.sort();
            return items.join(QLatin1Char(','));
        }
        if (value.typeId() == QMetaType::QVariantList) {
            QStringList items;
            const QVariantList values = value.toList();
            items.reserve(values.size());
            for (const QVariant& item : values)
                items.push_back(item.toString());
            items.sort();
            return items.join(QLatin1Char(','));
        }
        if (value.typeId() == QMetaType::Bool)
            return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        return value.toString();
    }

    QString querySignature(const QVariantMap& query)
    {
        QStringList keys = query.keys();
        keys.sort();

        QStringList parts;
        parts.reserve(keys.size());
        for (const QString& key : keys) {
            if (key.isEmpty())
                continue;
            const QVariant value = query.value(key);
            if (!value.isValid() || value.isNull())
                continue;
            const QString encodedKey = QString::fromLatin1(QUrl::toPercentEncoding(key));
            const QString encodedValue = QString::fromLatin1(QUrl::toPercentEncoding(queryValueSignature(value)));
            parts.push_back(QStringLiteral("%1=%2").arg(encodedKey, encodedValue));
        }
        return parts.join(QLatin1Char('&'));
    }

    QString serverPath(QString basePath, const QStringList& segments)
    {
        while (basePath.endsWith(QLatin1Char('/')))
            basePath.chop(1);
        for (const QString& segment : segments) {
            if (!segment.isEmpty())
                basePath += QLatin1Char('/') + QString::fromLatin1(QUrl::toPercentEncoding(segment));
        }
        return basePath.isEmpty() ? QStringLiteral("/") : basePath;
    }

}

BrowseDescriptor BrowseDescriptor::library(QString libraryId, QString collectionType, QString name)
{
    BrowseDescriptor descriptor;
    descriptor.kind = BrowseKind::Library;
    descriptor.id = libraryId;
    descriptor.collectionType = collectionType;
    descriptor.name = name;
    return descriptor;
}

BrowseDescriptor BrowseDescriptor::folderChildren(QString folderId, QString name)
{
    BrowseDescriptor descriptor;
    descriptor.kind = BrowseKind::FolderChildren;
    descriptor.id = folderId;
    descriptor.name = name;
    return descriptor;
}

BrowseDescriptor BrowseDescriptor::person(QString personId, QString name)
{
    BrowseDescriptor descriptor;
    descriptor.kind = BrowseKind::Person;
    descriptor.id = personId;
    descriptor.name = name;
    return descriptor;
}

BrowseDescriptor BrowseDescriptor::genre(QString name, QString collectionType)
{
    BrowseDescriptor descriptor;
    descriptor.kind = BrowseKind::Genre;
    descriptor.name = name;
    descriptor.collectionType = collectionType;
    return descriptor;
}

BrowseDescriptor BrowseDescriptor::studio(QString name)
{
    BrowseDescriptor descriptor;
    descriptor.kind = BrowseKind::Studio;
    descriptor.name = name;
    return descriptor;
}

BrowseDescriptor BrowseDescriptor::seriesSeasons(QString seriesId, QString seriesName)
{
    BrowseDescriptor descriptor;
    descriptor.kind = BrowseKind::SeriesSeasons;
    descriptor.id = seriesId;
    descriptor.seriesId = seriesId;
    descriptor.name = seriesName;
    return descriptor;
}

BrowseDescriptor BrowseDescriptor::seasonEpisodes(QString seriesId, QString seasonId, QString seasonName)
{
    BrowseDescriptor descriptor;
    descriptor.kind = BrowseKind::SeasonEpisodes;
    descriptor.id = seasonId.isEmpty() ? seriesId : seasonId;
    descriptor.seriesId = seriesId;
    descriptor.seasonId = seasonId;
    descriptor.name = seasonName;
    return descriptor;
}

BrowseDescriptor BrowseDescriptor::playlist(QString playlistId, QString name)
{
    BrowseDescriptor descriptor;
    descriptor.kind = BrowseKind::Playlist;
    descriptor.id = playlistId;
    descriptor.name = name;
    return descriptor;
}

BrowseDescriptor BrowseDescriptor::boxSet(QString boxSetId, QString name)
{
    BrowseDescriptor descriptor;
    descriptor.kind = BrowseKind::BoxSet;
    descriptor.id = boxSetId;
    descriptor.name = name;
    return descriptor;
}

BrowseDescriptor BrowseDescriptor::artistAlbums(QString artistId, QString artistName)
{
    BrowseDescriptor descriptor;
    descriptor.kind = BrowseKind::ArtistAlbums;
    descriptor.id = artistId;
    descriptor.name = artistName;
    return descriptor;
}

bool BrowseDescriptor::isValid() const
{
    switch (kind) {
    case BrowseKind::Genre:
    case BrowseKind::Studio:
        return !name.trimmed().isEmpty();
    case BrowseKind::SeasonEpisodes:
        return !seriesId.isEmpty();
    case BrowseKind::Library:
    case BrowseKind::FolderChildren:
    case BrowseKind::Person:
    case BrowseKind::SeriesSeasons:
    case BrowseKind::Playlist:
    case BrowseKind::BoxSet:
    case BrowseKind::ArtistAlbums:
        return !id.isEmpty();
    case BrowseKind::None:
        break;
    }
    return false;
}

QString BrowseDescriptor::kindKey() const
{
    return browseKindKey(kind);
}

QString BrowseDescriptor::cacheKey(const QVariantMap& query) const
{
    QString key = kindKey();
    if (!seriesId.isEmpty())
        key += QStringLiteral("/series/%1").arg(seriesId);
    if (!id.isEmpty() && id != seriesId)
        key += QStringLiteral("/%1").arg(id);
    if (!seasonId.isEmpty() && seasonId != id)
        key += QStringLiteral("/season/%1").arg(seasonId);
    if (id.isEmpty() && !name.isEmpty())
        key += QStringLiteral("/%1").arg(name);
    if (kind == BrowseKind::Library && !collectionType.isEmpty())
        key += QStringLiteral("/%1").arg(collectionType);

    const QString signature = querySignature(query);
    return signature.isEmpty() ? key : QStringLiteral("%1?%2").arg(key, signature);
}

QString exceptionMessage(const std::exception_ptr& exception)
{
    if (!exception)
        return QStringLiteral("Unknown error");

    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        return QString::fromUtf8(error.what());
    } catch (...) {
        return QStringLiteral("Unknown error");
    }
}

QString normalizedAudioOutputMode(const QString& mode)
{
    return normalizedPlatformAudioOutputMode(mode);
}

QString sanitizedDiagnosticUrl(QString url, qsizetype maxLength)
{
    static const QRegularExpression credentialQuery(
        QStringLiteral("((?:[?&]|%26)(?:secret|code|password|pw|api[_-]?key|access[_-]?token|token|x-emby-token)"
                       "(?:=|%3d))[^&%\\s]+"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression credentialField(
        QStringLiteral("((?:\\\"?(?:secret|code|password|pw|api[_-]?key|access[_-]?token|token|x-emby-token)\\\"?)"
                       "\\s*[:=]\\s*\\\"?)[^\\\",}\\s]+"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression authorizationHeader(
        QStringLiteral("((?:Authorization|X-Emby-Token)\\s*[:=]\\s*)[^,\\r\\n}]+"),
        QRegularExpression::CaseInsensitiveOption);
    url.replace(credentialQuery, QStringLiteral("\\1<redacted:credential>"));
    url.replace(credentialField, QStringLiteral("\\1<redacted:credential>"));
    url.replace(authorizationHeader, QStringLiteral("\\1<redacted:credential>"));
    return maxLength >= 0 ? url.left(maxLength) : url;
}

QString sanitizedLogMessage(QString message)
{
    message = sanitizedDiagnosticUrl(std::move(message));
    static const QRegularExpression personalField(
        QStringLiteral("((?:\\\"?(?:title|episode[_-]?name|library[_-]?name|user[_-]?name|server[_-]?url|address|"
                       "item[_-]?id|profile[_-]?id|cache[_-]?key|device[_-]?id)\\\"?)\\s*[:=]\\s*\\\"?)[^\\\",}\\s]+"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression url(
        QStringLiteral("\\bhttps?://[^\\s\\\"'<>]+"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression ipv4(
        QStringLiteral("(?<![0-9])(?:[0-9]{1,3}\\.){3}[0-9]{1,3}(?::[0-9]{1,5})?(?![0-9])"));
    static const QRegularExpression bracketedIpv6(QStringLiteral("\\[[0-9A-Fa-f:]+\\](?::[0-9]{1,5})?"));
    static const QRegularExpression stableId(
        QStringLiteral("\\b(?:[0-9a-fA-F]{32}|[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12})\\b"));
    message.replace(personalField, QStringLiteral("\\1<redacted:personal>"));
    message.replace(url, QStringLiteral("<redacted:url>"));
    message.replace(ipv4, QStringLiteral("<redacted:address>"));
    message.replace(bracketedIpv6, QStringLiteral("<redacted:address>"));
    message.replace(stableId, QStringLiteral("<redacted:id>"));
    return message;
}

QUrl serverUrlWithPath(const QString& serverUrl, const QStringList& segments)
{
    QUrl url(serverUrl);
    url.setPath(serverPath(url.path(), segments), QUrl::StrictMode);
    return url;
}

bool MovieItem::isPlayable() const
{
    return isPlayableItem(*this);
}

QString MovieItem::subtitle() const
{
    return itemSubtitle(*this);
}

bool isPlayableItem(const MovieItem& item)
{
    if (item.isVirtualItem || item.locationType.compare(QStringLiteral("Virtual"), Qt::CaseInsensitive) == 0)
        return false;
    return item.itemType == QStringLiteral("Movie") || item.itemType == QStringLiteral("Episode")
        || item.itemType == QStringLiteral("MusicVideo") || item.itemType == QStringLiteral("Video")
        || item.itemType == QStringLiteral("Audio") || item.itemType == QStringLiteral("AudioBook")
        || item.itemType == QStringLiteral("Trailer");
}

QString itemSubtitle(const MovieItem& item)
{
    if (item.itemType == QStringLiteral("Series")) {
        if (!item.episodeLabel.isEmpty())
            return item.episodeLabel;
        if (item.year <= 0)
            return QStringLiteral("Series");
        if (item.status.compare(QStringLiteral("Continuing"), Qt::CaseInsensitive) == 0)
            return QStringLiteral("%1 - Present").arg(item.year);
        const int endYear = QDate::fromString(item.endDate.left(10), Qt::ISODate).year();
        return endYear > item.year ? QStringLiteral("%1 - %2").arg(item.year).arg(endYear) : QString::number(item.year);
    }
    if (item.itemType == QStringLiteral("Season"))
        return QStringLiteral("Season");
    if (item.itemType == QStringLiteral("Episode")) {
        if (!item.episodeLabel.isEmpty())
            return item.episodeLabel;
        if (item.seasonNumber > 0 && item.episodeNumber > 0) {
            return QStringLiteral("S%1:E%2")
                .arg(item.seasonNumber, 2, 10, QLatin1Char('0'))
                .arg(item.episodeNumber, 2, 10, QLatin1Char('0'));
        }
        return item.episodeNumber > 0 ? QStringLiteral("Episode %1").arg(item.episodeNumber)
                                      : QStringLiteral("Episode");
    }
    return item.year > 0 ? QString::number(item.year) : QString();
}

QString itemDisplaySubtitle(const MovieItem& item)
{
    const QString subtitle = itemSubtitle(item);
    if (item.itemType == QStringLiteral("Episode") && !item.title.isEmpty())
        return subtitle.isEmpty() ? item.title : QStringLiteral("%1 · %2").arg(subtitle, item.title);
    return subtitle;
}

QString itemEpisodeCode(const MovieItem& item)
{
    if (item.itemType != QStringLiteral("Episode") || item.seasonNumber <= 0 || item.episodeNumber <= 0)
        return {};
    return QStringLiteral("S%1E%2")
        .arg(item.seasonNumber, 2, 10, QLatin1Char('0'))
        .arg(item.episodeNumber, 2, 10, QLatin1Char('0'));
}

bool isGenericEpisodeTitle(const MovieItem& item)
{
    if (item.itemType != QStringLiteral("Episode") || item.episodeNumber <= 0)
        return false;
    const QString title = item.title.simplified();
    constexpr QLatin1StringView prefix("Episode ");
    if (!title.startsWith(prefix, Qt::CaseInsensitive))
        return false;
    bool validNumber = false;
    const int titleNumber = title.sliced(prefix.size()).toInt(&validNumber);
    return validNumber && titleNumber == item.episodeNumber;
}

int episodicPlaybackStartIndex(const std::vector<MovieItem>& episodes)
{
    for (int index = 0; index < static_cast<int>(episodes.size()); ++index) {
        const MovieItem& episode = episodes[static_cast<size_t>(index)];
        if (!episode.played && episode.resumeTicks > 0 && isPlayableItem(episode))
            return index;
    }

    int lastPlayed = -1;
    for (int index = 0; index < static_cast<int>(episodes.size()); ++index) {
        if (episodes[static_cast<size_t>(index)].played)
            lastPlayed = index;
    }
    for (int index = lastPlayed + 1; index < static_cast<int>(episodes.size()); ++index) {
        if (isPlayableItem(episodes[static_cast<size_t>(index)]))
            return index;
    }
    return -1;
}

QVariantMap formatMediaInfo(const MovieItem& item, const QString& preferredAudioLanguage)
{
    if (item.id.isEmpty())
        return {};

    QVariantMap info { { QStringLiteral("date"), releaseDateLabel(item.premiereDate) },
        { QStringLiteral("runtime"), runtimeLabel(item.runtimeTicks) } };

    const MediaSourceInfo *source = nullptr;
    for (const MediaSourceInfo& candidate : item.mediaSources) {
        if (!candidate.streams.isEmpty()) {
            source = &candidate;
            break;
        }
    }
    if (!source)
        return info;

    const MediaStreamInfo *selectedAudio = nullptr;
    const MediaStreamInfo *selectedVideo = nullptr;
    const MediaStreamInfo *defaultAudio = nullptr;
    QStringList audioLanguages;
    QStringList subtitleLanguages;
    int audioCount = 0;
    QString resolution;
    for (const MediaStreamInfo& stream : source->streams) {
        if (stream.type.compare(QStringLiteral("Video"), Qt::CaseInsensitive) == 0) {
            if (!selectedVideo)
                selectedVideo = &stream;
            if (resolution.isEmpty())
                resolution = resolutionLabel(stream.width, stream.height);
        } else if (stream.type.compare(QStringLiteral("Audio"), Qt::CaseInsensitive) == 0) {
            ++audioCount;
            const QString tag = languageTag(stream.language);
            if (tag != QStringLiteral("UND") && !audioLanguages.contains(tag))
                audioLanguages.push_back(tag);
            if (!selectedAudio && streamLanguagesMatch(preferredAudioLanguage, stream.language))
                selectedAudio = &stream;
            if (!defaultAudio || (stream.isDefault && !defaultAudio->isDefault))
                defaultAudio = &stream;
        } else if (stream.type.compare(QStringLiteral("Subtitle"), Qt::CaseInsensitive) == 0) {
            if (!subtitleLanguages.contains(languageTag(stream.language)))
                subtitleLanguages.push_back(languageTag(stream.language));
        }
    }
    if (!selectedAudio)
        selectedAudio = defaultAudio;

    info.insert(QStringLiteral("resolution"), resolution);
    if (selectedVideo)
        info.insert(QStringLiteral("hdr"), hdrFormatLabel(*selectedVideo));

    if (selectedAudio) {
        const QString selectedLanguage = languageTag(selectedAudio->language);
        if (selectedLanguage != QStringLiteral("UND")) {
            audioLanguages.removeOne(selectedLanguage);
            audioLanguages.prepend(selectedLanguage);
        }
        QStringList parts;
        if (!audioLanguages.isEmpty())
            parts.push_back(audioLanguages.join(QLatin1Char(' ')));
        const QString layout = channelLayoutLabel(selectedAudio->channels);
        if (!layout.isEmpty())
            parts.push_back(layout);
        if (audioCount > 1)
            parts.push_back(QStringLiteral("×%1").arg(audioCount));
        info.insert(QStringLiteral("audio"), parts.join(QLatin1Char(' ')));
    }

    if (!subtitleLanguages.isEmpty()) {
        const qsizetype shown = std::min<qsizetype>(4, subtitleLanguages.size());
        QString subs = subtitleLanguages.mid(0, shown).join(QLatin1Char(' '));
        if (subtitleLanguages.size() > shown)
            subs += QStringLiteral(" +%1").arg(subtitleLanguages.size() - shown);
        info.insert(QStringLiteral("subs"), subs);
    }
    return info;
}

bool isMeaningfulResumePosition(qint64 resumeTicks, qint64 runtimeTicks)
{
    if (resumeTicks < 5 * kTicksPerSecond)
        return false;
    if (runtimeTicks <= 0)
        return true;

    const qint64 remainingTicks = runtimeTicks - resumeTicks;
    return resumeTicks < runtimeTicks && resumeTicks * 100 < runtimeTicks * 95 && remainingTicks > 30 * kTicksPerSecond;
}

qint64 normalizedResumeTicks(qint64 resumeTicks, qint64 runtimeTicks)
{
    return isMeaningfulResumePosition(resumeTicks, runtimeTicks) ? resumeTicks : 0;
}

} // namespace Spool
