#include "LocalProvider.h"

#include "../../common/AsyncTask.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <stdexcept>

namespace Spool {

namespace {

    constexpr auto kLibraryId = "local";

    // What mpv opens without help. Playlists and subtitle sidecars are left
    // out: they are not items, and mpv finds the sidecars itself.
    const QSet<QString>& videoSuffixes()
    {
        static const QSet<QString> suffixes { QStringLiteral("mkv"), QStringLiteral("mp4"), QStringLiteral("webm"),
            QStringLiteral("mov"), QStringLiteral("avi"), QStringLiteral("m4v"), QStringLiteral("ts"),
            QStringLiteral("mpg"), QStringLiteral("mpeg"), QStringLiteral("wmv") };
        return suffixes;
    }

    const QSet<QString>& audioSuffixes()
    {
        static const QSet<QString> suffixes { QStringLiteral("mp3"), QStringLiteral("flac"), QStringLiteral("ogg"),
            QStringLiteral("opus"), QStringLiteral("m4a"), QStringLiteral("wav"), QStringLiteral("aac") };
        return suffixes;
    }

    bool titleLess(const MovieItem& left, const MovieItem& right)
    {
        return left.title.localeAwareCompare(right.title) < 0;
    }

}

// The PlaybackSource half. Kept as a separate QObject because a provider is
// already one and moc allows a class one QObject base.
class LocalProvider::Playback final : public PlaybackSource {
public:
    explicit Playback(LocalProvider *provider)
        : PlaybackSource(provider)
        , m_provider(provider)
    {
    }

    QByteArray mediaRequestHeaders() const override
    {
        return {};
    }
    QUrl mediaOrigin() const override
    {
        return {};
    }
    int playbackParallelRequests() const override
    {
        return 1;
    }
    bool signedIn() const override
    {
        return true;
    }
    QString trickplayTileUrl(const QString&, int, int) const override
    {
        return {};
    }
    QCoro::Task<PlaybackSession> resolvePlayback(MovieItem item, bool) override
    {
        co_return m_provider->playbackSession(item.id);
    }
    QCoro::Task<std::vector<MediaSegment>> fetchMediaSegments(QString) override
    {
        co_return {};
    }
    QCoro::Task<std::vector<MovieItem>> fetchSeriesEpisodes(QString) override
    {
        co_return {};
    }
    QCoro::Task<void> reportPlaybackStart(PlaybackSession, double, int, bool) override
    {
        co_return;
    }
    QCoro::Task<void> reportPlaybackProgress(PlaybackSession, qint64, bool, double, int, bool) override
    {
        co_return;
    }
    QCoro::Task<void> reportPlaybackStopped(PlaybackSession, qint64, bool, double) override
    {
        co_return;
    }

private:
    LocalProvider *m_provider;
};

LocalProvider::LocalProvider(QString accountId, QString libraryRoot, QObject *parent)
    : Provider(parent)
    , m_accountId(std::move(accountId))
    , m_root(QDir(std::move(libraryRoot)).absolutePath())
    , m_playback(new Playback(this))
{
    m_libraryName = QDir(m_root).dirName();
    if (m_libraryName.isEmpty())
        m_libraryName = QStringLiteral("Local files");
    // A large folder takes a while to walk; do it off the GUI thread and
    // announce the library once it is known.
    auto *watcher = new QFutureWatcher<std::vector<Record>>(this);
    connect(watcher, &QFutureWatcher<std::vector<Record>>::finished, this, [this, watcher] {
        setRecords(watcher->result());
        watcher->deleteLater();
        emit contentChanged({});
    });
    watcher->setFuture(Async::background([root = m_root] { return LocalProvider::scanFolder(root); }));
}

LocalProvider::~LocalProvider() = default;

QString LocalProvider::id() const
{
    return m_accountId;
}

QString LocalProvider::displayName() const
{
    return QStringLiteral("Local files");
}

Provider::Capabilities LocalProvider::capabilities() const
{
    return Search | UserItemState;
}

PlaybackSource *LocalProvider::playback()
{
    return m_playback;
}

void LocalProvider::scan()
{
    setRecords(scanFolder(m_root));
}

void LocalProvider::setRecords(std::vector<Record> records)
{
    m_records = std::move(records);
    m_index.clear();
    for (size_t index = 0; index < m_records.size(); ++index)
        m_index.insert(m_records[index].item.id, index);
}

std::vector<LocalProvider::Record> LocalProvider::scanFolder(const QString& folder)
{
    std::vector<Record> records;
    const QDir root(folder);
    QDirIterator files(folder, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QFileInfo info(files.next());
        const QString suffix = info.suffix().toLower();
        const bool video = videoSuffixes().contains(suffix);
        if (!video && !audioSuffixes().contains(suffix))
            continue;
        Record record;
        record.path = info.absoluteFilePath();
        record.modified = info.lastModified();
        MovieItem& item = record.item;
        item.id = root.relativeFilePath(record.path);
        item.title = info.completeBaseName();
        item.itemType = video ? QStringLiteral("Movie") : QStringLiteral("Audio");
        item.path = record.path;
        item.dateCreated = record.modified.toString(Qt::ISODate);
        MediaSourceInfo source;
        source.id = item.id;
        source.name = info.fileName();
        source.path = record.path;
        source.container = suffix;
        source.protocol = QStringLiteral("File");
        source.size = info.size();
        item.mediaSources.push_back(source);
        records.push_back(std::move(record));
    }
    std::sort(records.begin(), records.end(),
        [](const Record& left, const Record& right) { return titleLess(left.item, right.item); });
    return records;
}

QString LocalProvider::libraryScopeKey() const
{
    return QStringLiteral("local:") + m_root;
}

const LocalProvider::Record *LocalProvider::record(const QString& itemId) const
{
    const auto found = m_index.constFind(itemId);
    return found == m_index.cend() ? nullptr : &m_records[*found];
}

LocalProvider::Record *LocalProvider::record(const QString& itemId)
{
    const auto found = m_index.constFind(itemId);
    return found == m_index.cend() ? nullptr : &m_records[*found];
}

std::vector<MovieItem> LocalProvider::items(int startIndex, int limit) const
{
    std::vector<MovieItem> page;
    const size_t begin = static_cast<size_t>(std::max(0, startIndex));
    const size_t end = limit > 0 ? std::min(m_records.size(), begin + static_cast<size_t>(limit)) : m_records.size();
    for (size_t index = begin; index < end; ++index)
        page.push_back(m_records[index].item);
    return page;
}

QCoro::Task<PagedMovieItems> LocalProvider::fetchBrowsePage(
    BrowseDescriptor descriptor, int startIndex, int limit, QVariantMap)
{
    PagedMovieItems page;
    page.startIndex = startIndex;
    page.limit = limit;
    // One flat library: the folder's own listing and anything that asks for
    // its children. Every other browse shape (people, seasons, genres) has
    // nothing behind it here.
    const bool wholeLibrary = descriptor.kind == BrowseKind::Library && descriptor.id == QLatin1String(kLibraryId);
    if (wholeLibrary || descriptor.kind == BrowseKind::FolderChildren) {
        page.items = items(startIndex, limit);
        page.totalRecordCount = static_cast<int>(m_records.size());
    }
    co_return page;
}

QCoro::Task<MovieItem> LocalProvider::fetchItemDetails(QString itemId)
{
    if (const Record *found = record(itemId))
        co_return found->item;
    throw std::runtime_error("No such file in the library.");
}

QCoro::Task<std::vector<MovieItem>> LocalProvider::fetchSeasons(QString)
{
    co_return {};
}

QCoro::Task<std::vector<MovieItem>> LocalProvider::fetchEpisodes(QString, QString)
{
    co_return {};
}

QCoro::Task<std::vector<MovieItem>> LocalProvider::fetchResumeItems(int limit)
{
    std::vector<MovieItem> resumable;
    for (const Record& entry : m_records) {
        if (entry.item.resumeTicks > 0 && !entry.item.played)
            resumable.push_back(entry.item);
        if (limit > 0 && static_cast<int>(resumable.size()) >= limit)
            break;
    }
    co_return resumable;
}

QCoro::Task<std::vector<MovieItem>> LocalProvider::fetchNextUpEpisodes(int)
{
    co_return {};
}

QCoro::Task<std::vector<MovieItem>> LocalProvider::fetchLatestItems(QString, int limit)
{
    std::vector<const Record *> byDate;
    byDate.reserve(m_records.size());
    for (const Record& entry : m_records)
        byDate.push_back(&entry);
    std::sort(byDate.begin(), byDate.end(),
        [](const Record *left, const Record *right) { return left->modified > right->modified; });
    std::vector<MovieItem> latest;
    for (const Record *entry : byDate) {
        if (limit > 0 && static_cast<int>(latest.size()) >= limit)
            break;
        latest.push_back(entry->item);
    }
    co_return latest;
}

QCoro::Task<std::vector<MovieItem>> LocalProvider::fetchSimilarItems(QString, int)
{
    co_return {};
}

QCoro::Task<PersonCredits> LocalProvider::fetchItemsByPerson(QString, int)
{
    co_return {};
}

QCoro::Task<std::vector<LibraryItem>> LocalProvider::fetchLibraries()
{
    LibraryItem library;
    library.id = QLatin1String(kLibraryId);
    library.name = m_libraryName;
    library.collectionType = QStringLiteral("movies");
    co_return std::vector<LibraryItem> { library };
}

QCoro::Task<QVariantMap> LocalProvider::fetchLibraryFilterOptions(QString, QString)
{
    co_return {};
}

QCoro::Task<std::vector<MovieItem>> LocalProvider::fetchItemsByIds(QStringList itemIds)
{
    std::vector<MovieItem> found;
    for (const QString& itemId : itemIds) {
        if (const Record *entry = record(itemId))
            found.push_back(entry->item);
    }
    co_return found;
}

QCoro::Task<std::vector<MovieItem>> LocalProvider::searchItems(QString searchTerm, int limit)
{
    const QString term = searchTerm.trimmed();
    std::vector<MovieItem> matches;
    for (const Record& entry : m_records) {
        if (limit > 0 && static_cast<int>(matches.size()) >= limit)
            break;
        if (term.isEmpty() || entry.item.title.contains(term, Qt::CaseInsensitive))
            matches.push_back(entry.item);
    }
    co_return matches;
}

QCoro::Task<std::vector<MovieItem>> LocalProvider::fetchSearchSuggestions(int limit)
{
    co_return items(0, limit);
}

QCoro::Task<void> LocalProvider::setItemFavorite(QString itemId, bool favorite)
{
    if (Record *entry = record(itemId))
        entry->item.favorite = favorite;
    co_return;
}

QCoro::Task<void> LocalProvider::setItemPlayed(QString itemId, bool played)
{
    if (Record *entry = record(itemId)) {
        entry->item.played = played;
        if (played) {
            entry->item.resumeTicks = 0;
            ++entry->item.playCount;
        }
    }
    co_return;
}

QCoro::Task<void> LocalProvider::setItemPlaybackPosition(QString itemId, qint64 positionTicks)
{
    if (Record *entry = record(itemId))
        entry->item.resumeTicks = std::max<qint64>(0, positionTicks);
    co_return;
}

PlaybackSession LocalProvider::playbackSession(const QString& itemId) const
{
    const Record *entry = record(itemId);
    if (!entry)
        throw std::runtime_error("No such file in the library.");
    PlaybackSession session;
    session.itemId = entry->item.id;
    session.title = entry->item.title;
    session.itemType = entry->item.itemType;
    session.url = QUrl::fromLocalFile(entry->path).toString();
    session.mediaSourceId = entry->item.id;
    session.container = entry->item.mediaSources.front().container;
    session.startTimeTicks = entry->item.resumeTicks;
    session.runtimeTicks = entry->item.runtimeTicks;
    return session;
}

} // namespace Spool
