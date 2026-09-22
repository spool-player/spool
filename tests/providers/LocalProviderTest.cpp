#include "providers/local/LocalProvider.h"
#include "provider/Catalog.h"
#include "provider/PlaybackSource.h"
#include "provider/Provider.h"
#include "provider/SearchSource.h"
#include "provider/UserItemStateSink.h"

#include "TestMain.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QUrl>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool hasTitle(const std::vector<JellyfinNative::MovieItem>& items, const QString& title)
{
    for (const auto& item : items) {
        if (item.title == title)
            return true;
    }
    return false;
}

} // namespace

// The folder-of-files provider against the media fixtures: the one library,
// its page, details, search, item state and a playable session, all from
// nothing but a directory listing.
JELLYFIN_TEST_MAIN("local-provider")
{
    QCoreApplication app(argc, argv);
    using namespace JellyfinNative;

    const QString fixtures = QDir(QStringLiteral(TEST_SOURCE_DIR)).filePath(QStringLiteral("tests/media/fixtures"));
    LocalProvider provider(QStringLiteral("local-account"), fixtures);
    bool scanned = false;
    QObject::connect(&provider, &Provider::contentChanged, &app, [&scanned] { scanned = true; });
    require(provider.id() == QStringLiteral("local-account"), "the provider is named after its account");
    require(provider.ready(), "a folder needs no sign-in");
    require(provider.capabilities() == (Provider::Search | Provider::UserItemState),
        "a folder searches and remembers item state, nothing more");
    require(
        provider.catalog() && provider.playback() && provider.artwork() && provider.search() && provider.itemState(),
        "the required and the claimed optional interfaces are served");
    require(!provider.streamQuality(), "unclaimed capabilities have no object");

    // The folder is walked off the GUI thread; the library is announced once known.
    QElapsedTimer waited;
    waited.start();
    while (!scanned && waited.elapsed() < 5000)
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
    require(scanned, "the background scan announces the library");

    Catalog *catalog = provider.catalog();
    const auto libraries = QCoro::waitFor(catalog->fetchLibraries());
    require(libraries.size() == 1, "one library per folder");
    require(libraries.front().id == QStringLiteral("local"), "the library id is fixed");
    require(libraries.front().name == QStringLiteral("fixtures"), "the library is named after the folder");

    // Media files only: the playlist and the subtitle sidecar are not items.
    const auto page = QCoro::waitFor(
        catalog->fetchBrowsePage(BrowseDescriptor::library(libraries.front().id, libraries.front().collectionType)));
    require(page.totalRecordCount == 4, "four media files in the fixtures");
    require(page.items.size() == 4, "the first page holds them all");
    require(hasTitle(page.items, QStringLiteral("audio")), "the flac is listed");
    require(hasTitle(page.items, QStringLiteral("direct-mpeg2")), "the mkv is listed");
    require(hasTitle(page.items, QStringLiteral("remux-h264")), "the mp4 is listed");
    require(hasTitle(page.items, QStringLiteral("transcode-0")), "the transport stream is listed");
    require(!hasTitle(page.items, QStringLiteral("subtitle")) && !hasTitle(page.items, QStringLiteral("transcode")),
        "sidecars and playlists are not items");
    require(page.items.front().title == QStringLiteral("audio"), "items are sorted by title");
    require(page.items.front().itemType == QStringLiteral("Audio"), "audio files are audio items");
    require(page.items[1].itemType == QStringLiteral("Movie"), "video files are movies");
    require(page.items[1].isPlayable(), "video items are playable");

    const auto paged = QCoro::waitFor(
        catalog->fetchBrowsePage(BrowseDescriptor::library(QStringLiteral("local"), QStringLiteral("movies")), 1, 2));
    require(paged.items.size() == 2 && paged.startIndex == 1 && paged.totalRecordCount == 4, "paging is honoured");
    require(paged.items.front().title == QStringLiteral("direct-mpeg2"), "paging starts where asked");

    const auto other = QCoro::waitFor(catalog->fetchBrowsePage(BrowseDescriptor::person(QStringLiteral("nobody"))));
    require(other.items.empty() && other.totalRecordCount == 0, "browse shapes a folder cannot serve are empty");

    const MovieItem mkv = page.items[1];
    const MovieItem details = QCoro::waitFor(catalog->fetchItemDetails(mkv.id));
    require(details.id == mkv.id && details.title == mkv.title, "details return the listed item");
    require(details.mediaSources.size() == 1 && details.mediaSources.front().container == QStringLiteral("mkv"),
        "the file is its own media source");
    bool missingThrows = false;
    try {
        QCoro::waitFor(catalog->fetchItemDetails(QStringLiteral("nope.mkv")));
    } catch (const std::runtime_error&) {
        missingThrows = true;
    }
    require(missingThrows, "an unknown id is an error, not an empty item");

    const auto byIds = QCoro::waitFor(catalog->fetchItemsByIds({ mkv.id, QStringLiteral("nope.mkv") }));
    require(byIds.size() == 1 && byIds.front().id == mkv.id, "lookup by ids skips unknown ones");
    require(QCoro::waitFor(catalog->fetchLatestItems({}, 2)).size() == 2, "latest items honour the limit");
    require(QCoro::waitFor(catalog->fetchResumeItems()).empty(), "nothing is resumable before anything played");

    SearchSource *search = provider.search();
    const auto hits = QCoro::waitFor(search->searchItems(QStringLiteral("H264")));
    require(hits.size() == 1 && hits.front().title == QStringLiteral("remux-h264"), "search matches titles, any case");
    require(QCoro::waitFor(search->searchItems(QStringLiteral("zzz"))).empty(), "search misses cleanly");
    require(QCoro::waitFor(search->fetchSearchSuggestions(3)).size() == 3, "suggestions honour the limit");

    UserItemStateSink *state = provider.itemState();
    QCoro::waitFor(state->setItemPlaybackPosition(mkv.id, 5'000'000));
    require(QCoro::waitFor(catalog->fetchItemDetails(mkv.id)).resumeTicks == 5'000'000, "position is remembered");
    require(QCoro::waitFor(catalog->fetchResumeItems()).size() == 1, "a started item is resumable");
    QCoro::waitFor(state->setItemFavorite(mkv.id, true));
    require(QCoro::waitFor(catalog->fetchItemDetails(mkv.id)).favorite, "favourite is remembered");
    QCoro::waitFor(state->setItemPlayed(mkv.id, true));
    const MovieItem played = QCoro::waitFor(catalog->fetchItemDetails(mkv.id));
    require(played.played && played.resumeTicks == 0 && played.playCount == 1, "played clears the resume point");
    require(QCoro::waitFor(catalog->fetchResumeItems()).empty(), "a finished item is no longer resumable");

    PlaybackSource *playback = provider.playback();
    require(playback->signedIn() && playback->mediaRequestHeaders().isEmpty() && playback->mediaOrigin().isEmpty(),
        "local playback needs no headers and no trusted origin");
    const PlaybackSession session = QCoro::waitFor(playback->resolvePlayback(mkv, false));
    require(session.itemId == mkv.id && session.title == mkv.title, "the session names the item");
    const QUrl url(session.url);
    require(url.isLocalFile() && url.toLocalFile() == QDir(fixtures).filePath(QStringLiteral("direct-mpeg2.mkv")),
        "the session URL is the file itself");
    require(session.container == QStringLiteral("mkv"), "the container is the file's suffix");
    require(QCoro::waitFor(playback->fetchMediaSegments(mkv.id)).empty(), "a folder has no segments");
    require(provider.artwork()->imageUrl({ mkv.id, {}, QStringLiteral("Primary"), 300 }).isEmpty(),
        "a folder has no artwork");

    std::cout << "local provider ok\n";
    return 0;
}
