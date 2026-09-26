#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

#include <exception>
#include <vector>

namespace Spool {

struct LibraryItem {
    Q_GADGET
    Q_PROPERTY(QString id MEMBER id)
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString collectionType MEMBER collectionType)
    Q_PROPERTY(QString imageTag MEMBER imageTag)

public:
    QString id;
    QString name;
    QString collectionType;
    QString imageTag;
};

enum class BrowseKind {
    None,
    Library,
    FolderChildren,
    Person,
    Genre,
    Studio,
    SeriesSeasons,
    SeasonEpisodes,
    Playlist,
    BoxSet,
    ArtistAlbums,
};

struct BrowseDescriptor {
    BrowseKind kind = BrowseKind::None;
    QString id;
    QString name;
    QString collectionType;
    QString seriesId;
    QString seasonId;

    static BrowseDescriptor library(QString libraryId, QString collectionType, QString name = {});
    static BrowseDescriptor folderChildren(QString folderId, QString name = {});
    static BrowseDescriptor person(QString personId, QString name = {});
    static BrowseDescriptor genre(QString name, QString collectionType = {});
    static BrowseDescriptor studio(QString name);
    static BrowseDescriptor seriesSeasons(QString seriesId, QString seriesName = {});
    static BrowseDescriptor seasonEpisodes(QString seriesId, QString seasonId = {}, QString seasonName = {});
    static BrowseDescriptor playlist(QString playlistId, QString name = {});
    static BrowseDescriptor boxSet(QString boxSetId, QString name = {});
    static BrowseDescriptor artistAlbums(QString artistId, QString artistName = {});

    bool isValid() const;
    QString kindKey() const;
    QString cacheKey(const QVariantMap& query = {}) const;
};

struct PersonItem {
    Q_GADGET
    Q_PROPERTY(QString id MEMBER id)
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString type MEMBER type)
    Q_PROPERTY(QString role MEMBER role)
    Q_PROPERTY(QString imageTag MEMBER imageTag)

public:
    QString id;
    QString name;
    QString type;
    QString role;
    QString imageTag;

    friend bool operator==(const PersonItem&, const PersonItem&) = default;
};

struct MediaStreamInfo {
    Q_GADGET
    Q_PROPERTY(int index MEMBER index)
    Q_PROPERTY(QString type MEMBER type)
    Q_PROPERTY(QString codec MEMBER codec)
    Q_PROPERTY(QString profile MEMBER profile)
    Q_PROPERTY(QString displayTitle MEMBER displayTitle)
    Q_PROPERTY(QString title MEMBER title)
    Q_PROPERTY(QString language MEMBER language)
    Q_PROPERTY(QString pixelFormat MEMBER pixelFormat)
    Q_PROPERTY(QString videoRange MEMBER videoRange)
    Q_PROPERTY(QString videoRangeType MEMBER videoRangeType)
    Q_PROPERTY(QString colorPrimaries MEMBER colorPrimaries)
    Q_PROPERTY(QString colorTransfer MEMBER colorTransfer)
    Q_PROPERTY(QString colorSpace MEMBER colorSpace)
    Q_PROPERTY(QString aspectRatio MEMBER aspectRatio)
    Q_PROPERTY(int width MEMBER width)
    Q_PROPERTY(int height MEMBER height)
    Q_PROPERTY(double frameRate MEMBER frameRate)
    Q_PROPERTY(int bitRate MEMBER bitRate)
    Q_PROPERTY(int bitDepth MEMBER bitDepth)
    Q_PROPERTY(int channels MEMBER channels)
    Q_PROPERTY(int sampleRate MEMBER sampleRate)
    Q_PROPERTY(bool isDefault MEMBER isDefault)
    Q_PROPERTY(bool isForced MEMBER isForced)
    Q_PROPERTY(bool isExternal MEMBER isExternal)
    Q_PROPERTY(bool isInterlaced MEMBER isInterlaced)

public:
    int index = -1;
    QString type;
    QString codec;
    QString profile;
    QString displayTitle;
    QString title;
    QString language;
    QString pixelFormat;
    QString videoRange;
    QString videoRangeType;
    QString colorPrimaries;
    QString colorTransfer;
    QString colorSpace;
    QString aspectRatio;
    int width = 0;
    int height = 0;
    double frameRate = 0.0;
    int bitRate = 0;
    int bitDepth = 0;
    int channels = 0;
    int sampleRate = 0;
    bool isDefault = false;
    bool isForced = false;
    bool isExternal = false;
    bool isInterlaced = false;

    friend bool operator==(const MediaStreamInfo&, const MediaStreamInfo&) = default;
};

struct MediaSourceInfo {
    Q_GADGET
    Q_PROPERTY(QString id MEMBER id)
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString path MEMBER path)
    Q_PROPERTY(QString container MEMBER container)
    Q_PROPERTY(QString protocol MEMBER protocol)
    Q_PROPERTY(QString videoType MEMBER videoType)
    Q_PROPERTY(qint64 size MEMBER size)
    Q_PROPERTY(int bitRate MEMBER bitRate)
    Q_PROPERTY(qint64 runtimeTicks MEMBER runtimeTicks)
    Q_PROPERTY(QList<Spool::MediaStreamInfo> streams MEMBER streams)

public:
    QString id;
    QString name;
    QString path;
    QString container;
    QString protocol;
    QString videoType;
    qint64 size = 0;
    int bitRate = 0;
    qint64 runtimeTicks = 0;
    QList<MediaStreamInfo> streams;

    friend bool operator==(const MediaSourceInfo&, const MediaSourceInfo&) = default;
};

struct ExternalUrlInfo {
    Q_GADGET
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString url MEMBER url)

public:
    QString name;
    QString url;

    friend bool operator==(const ExternalUrlInfo&, const ExternalUrlInfo&) = default;
};

struct MovieItem {
    Q_GADGET
    Q_PROPERTY(QString movieId MEMBER id)
    Q_PROPERTY(QString title MEMBER title)
    Q_PROPERTY(QString sortName MEMBER sortName)
    Q_PROPERTY(QString overview MEMBER overview)
    Q_PROPERTY(QString posterTag MEMBER posterTag)
    Q_PROPERTY(QString itemType MEMBER itemType)
    Q_PROPERTY(QString imdbId MEMBER imdbId)
    Q_PROPERTY(QString tmdbId MEMBER tmdbId)
    Q_PROPERTY(QString playlistItemId MEMBER playlistItemId)
    Q_PROPERTY(QString locationType MEMBER locationType)
    Q_PROPERTY(bool isVirtualItem MEMBER isVirtualItem)
    Q_PROPERTY(QString seriesId MEMBER seriesId)
    Q_PROPERTY(QString seasonId MEMBER seasonId)
    Q_PROPERTY(QString seriesName MEMBER seriesName)
    Q_PROPERTY(QString album MEMBER album)
    Q_PROPERTY(QString albumId MEMBER albumId)
    Q_PROPERTY(QString albumArtist MEMBER albumArtist)
    Q_PROPERTY(QString albumPrimaryImageTag MEMBER albumPrimaryImageTag)
    Q_PROPERTY(QString seriesPrimaryImageTag MEMBER seriesPrimaryImageTag)
    Q_PROPERTY(QString path MEMBER path)
    Q_PROPERTY(int year MEMBER year)
    Q_PROPERTY(int seasonNumber MEMBER seasonNumber)
    Q_PROPERTY(int episodeNumber MEMBER episodeNumber)
    Q_PROPERTY(QString episodeLabel MEMBER episodeLabel)
    Q_PROPERTY(qint64 resumeTicks MEMBER resumeTicks)
    Q_PROPERTY(qint64 runtimeTicks MEMBER runtimeTicks)
    Q_PROPERTY(QString dateCreated MEMBER dateCreated)
    Q_PROPERTY(QString datePlayed MEMBER datePlayed)
    Q_PROPERTY(QString dateLastContentAdded MEMBER dateLastContentAdded)
    Q_PROPERTY(int playCount MEMBER playCount)
    Q_PROPERTY(bool playable READ isPlayable)
    Q_PROPERTY(QString subtitle READ subtitle)
    Q_PROPERTY(bool favorite MEMBER favorite)
    Q_PROPERTY(bool played MEMBER played)
    Q_PROPERTY(QString backdropTag MEMBER backdropTag)
    Q_PROPERTY(QString logoTag MEMBER logoTag)
    Q_PROPERTY(QString bannerTag MEMBER bannerTag)
    Q_PROPERTY(QString thumbTag MEMBER thumbTag)
    Q_PROPERTY(QStringList genres MEMBER genres)
    Q_PROPERTY(QStringList tags MEMBER tags)
    Q_PROPERTY(QStringList studios MEMBER studios)
    Q_PROPERTY(QString officialRating MEMBER officialRating)
    Q_PROPERTY(double communityRating MEMBER communityRating)
    Q_PROPERTY(double criticRating MEMBER criticRating)
    Q_PROPERTY(int recursiveItemCount MEMBER recursiveItemCount)
    Q_PROPERTY(QString premiereDate MEMBER premiereDate)
    Q_PROPERTY(QString endDate MEMBER endDate)
    Q_PROPERTY(QString status MEMBER status)
    Q_PROPERTY(QList<Spool::PersonItem> people MEMBER people)
    Q_PROPERTY(QList<Spool::MediaSourceInfo> mediaSources MEMBER mediaSources)
    Q_PROPERTY(QList<Spool::ExternalUrlInfo> externalUrls MEMBER externalUrls)

public:
    QString id;
    QString title;
    QString sortName;
    QString overview;
    QString posterTag;
    QString itemType;
    QString imdbId;
    QString tmdbId;
    QString playlistItemId;
    QString locationType;
    bool isVirtualItem = false;
    QString seriesId;
    QString seasonId;
    QString seriesName;
    QString seriesPrimaryImageTag;
    // Tracks carry their cover on the album rather than on themselves.
    QString album;
    QString albumId;
    QString albumArtist;
    QString albumPrimaryImageTag;
    QString path;
    int year = 0;
    int seasonNumber = 0;
    int episodeNumber = 0;
    QString episodeLabel;
    qint64 resumeTicks = 0;
    qint64 runtimeTicks = 0;
    QString dateCreated;
    QString datePlayed;
    QString dateLastContentAdded;
    int playCount = 0;
    bool favorite = false;
    bool played = false;
    QString backdropTag;
    QString logoTag;
    QString bannerTag;
    QString thumbTag;
    QStringList genres;
    QStringList tags;
    QStringList studios;
    QString officialRating;
    double communityRating = 0.0;
    double criticRating = 0.0;
    int recursiveItemCount = 0;
    QString premiereDate;
    QString endDate;
    QString status;
    QList<PersonItem> people;
    QList<MediaSourceInfo> mediaSources;
    QList<ExternalUrlInfo> externalUrls;

    bool isPlayable() const;
    QString subtitle() const;

    friend bool operator==(const MovieItem&, const MovieItem&) = default;
};

struct PersonCredits {
    std::vector<MovieItem> items;
    std::vector<MovieItem> relatedSeries;
};

bool isPlayableItem(const MovieItem& item);
QString itemSubtitle(const MovieItem& item);
QString itemDisplaySubtitle(const MovieItem& item);
QString itemEpisodeCode(const MovieItem& item);
bool isGenericEpisodeTitle(const MovieItem& item);
int episodicPlaybackStartIndex(const std::vector<MovieItem>& episodes);

struct MediaSegment {
    Q_GADGET
    Q_PROPERTY(QString id MEMBER id)
    Q_PROPERTY(QString type MEMBER type)
    Q_PROPERTY(qint64 startTicks MEMBER startTicks)
    Q_PROPERTY(qint64 endTicks MEMBER endTicks)

public:
    QString id;
    QString type; // "Intro", "Outro", "Recap", "Preview", "Commercial"
    qint64 startTicks = 0;
    qint64 endTicks = 0;
};

// Per-width trickplay manifest matching Jellyfin's
// item.Trickplay[mediaSourceId][width] structure.
struct TrickplayInfo {
    int width = 0;
    int height = 0;
    int tileWidth = 0; // tiles across in one image
    int tileHeight = 0; // tiles down in one image
    int thumbnailCount = 0;
    int intervalMs = 0;
    int bandwidth = 0;
};

struct SubtitlePreferences {
    QString language;
    QString audioLanguage;
    QString mode = QStringLiteral("Default");
    // Audio track auto-selection ("Default" or "Smart") rides along so both
    // track preferences reach the player through one apply path.
    QString audioMode = QStringLiteral("Default");
    QString styling = QStringLiteral("Auto");
    QString textWeight = QStringLiteral("normal");
    QString font;
    QString textColor = QStringLiteral("#ffffff");
    bool overrideTextColor = false;
    QString dropShadow;
    QString textBackground = QStringLiteral("transparent");
    int verticalPosition = 95;
    int scalePercent = 100;
    bool alwaysOverridePositionAndSize = false;
    int bitmapSharpnessPercent = 45;
    bool recolorImageSubtitles = false;
    bool bitmapShadowEnabled = true;
    int bitmapShadowCoreSize = 1;
    int bitmapShadowCoreGrow = 1;
    int bitmapShadowCoreOpacityPercent = 70;
    bool bitmapShadowSpreadEnabled = true;
    int bitmapShadowSpreadSize = 6;
    int bitmapShadowSpreadGrow = 0;
    int bitmapShadowSpreadX = 2;
    int bitmapShadowSpreadY = 3;
    int bitmapShadowSpreadOpacityPercent = 30;
    bool bitmapShadowDither = true;
    bool allowSubtitlesInBlackBars = true;
    int hdrBrightnessPercent = 50;

    friend bool operator==(const SubtitlePreferences&, const SubtitlePreferences&) = default;
};

struct PagedMovieItems {
    std::vector<MovieItem> items;
    int totalRecordCount = 0;
    int startIndex = 0;
    int limit = 0;
};

struct PlaybackQueueItem {
    QString itemId;
    QString playlistItemId;
};

struct PlaybackSession {
    QString itemId;
    QString title;
    QString itemType;
    QString url;
    QString mediaSourceId;
    QString playSessionId;
    QString playMethod = QStringLiteral("DirectPlay");
    QString container;
    qint64 startTimeTicks = 0;
    qint64 runtimeTicks = 0;
    QList<MediaStreamInfo> mediaStreams;
    std::vector<MediaSegment> segments;
    TrickplayInfo trickplay;
    std::vector<PlaybackQueueItem> nowPlayingQueue;
    int audioStreamIndex = -1;
    int subtitleStreamIndex = -1;
    bool codecFallback = false;
    bool restoreStreamSelection = false;
};

QString exceptionMessage(const std::exception_ptr& exception);
QString normalizedAudioOutputMode(const QString& mode);
QString sanitizedDiagnosticUrl(QString url, qsizetype maxLength = -1);
QString sanitizedLogMessage(QString message);
QUrl serverUrlWithPath(const QString& serverUrl, const QStringList& segments);
bool isMeaningfulResumePosition(qint64 resumeTicks, qint64 runtimeTicks);
qint64 normalizedResumeTicks(qint64 resumeTicks, qint64 runtimeTicks);
QVariantMap formatMediaInfo(const MovieItem& item, const QString& preferredAudioLanguage);

} // namespace Spool

Q_DECLARE_METATYPE(Spool::LibraryItem)
Q_DECLARE_METATYPE(Spool::PersonItem)
Q_DECLARE_METATYPE(Spool::MediaStreamInfo)
Q_DECLARE_METATYPE(Spool::MediaSourceInfo)
Q_DECLARE_METATYPE(Spool::ExternalUrlInfo)
Q_DECLARE_METATYPE(Spool::MovieItem)
Q_DECLARE_METATYPE(Spool::MediaSegment)
Q_DECLARE_METATYPE(QList<Spool::PersonItem>)
Q_DECLARE_METATYPE(QList<Spool::MediaStreamInfo>)
Q_DECLARE_METATYPE(QList<Spool::MediaSourceInfo>)
Q_DECLARE_METATYPE(QList<Spool::ExternalUrlInfo>)
