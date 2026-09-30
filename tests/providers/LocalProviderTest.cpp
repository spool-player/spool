#include "providers/local/LocalProvider.h"
#include "app/LocalThumbnail.h"
#include "cache/DatabaseManager.h"
#include "provider/Catalog.h"
#include "provider/PlaybackSource.h"
#include "provider/Provider.h"
#include "provider/ProviderRegistry.h"
#include "provider/ProviderUiContext.h"
#include "provider/SearchSource.h"
#include "provider/UserItemStateSink.h"

#include "TestMain.h"

#include <QCoroTask>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QSet>
#include <QTemporaryDir>
#include <QUrl>

#include <clocale>
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

bool hasTitle(const std::vector<Spool::MovieItem>& items, const QString& title)
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
SPOOL_TEST_MAIN("local-provider")
{
    QCoreApplication app(argc, argv);
    // Match the application's libmpv startup requirement.
    std::setlocale(LC_NUMERIC, "C");
    using namespace Spool;

    const QString fixtures = QDir(QStringLiteral(TEST_SOURCE_DIR)).filePath(QStringLiteral("tests/media/fixtures"));
    LocalProvider provider(QStringLiteral("local-account"), { fixtures });
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
    const auto page = QCoro::waitFor(catalog->fetchBrowsePage(
        BrowseDescriptor::library(libraries.front().id, libraries.front().collectionType), 0, 72, {}, std::nullopt));
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

    const auto paged = QCoro::waitFor(catalog->fetchBrowsePage(
        BrowseDescriptor::library(QStringLiteral("local"), QStringLiteral("movies")), 1, 2, {}, std::nullopt));
    require(paged.items.size() == 2 && paged.startIndex == 1 && paged.totalRecordCount == 4, "paging is honoured");
    require(paged.items.front().title == QStringLiteral("direct-mpeg2"), "paging starts where asked");
    require(!paged.exhausted && paged.nextCursor == QStringLiteral("3"), "local pages expose continuation");
    const auto last = QCoro::waitFor(catalog->fetchBrowsePage(
        BrowseDescriptor::library(QStringLiteral("local"), QStringLiteral("movies")), 3, 1, {}, paged.nextCursor));
    require(last.items.size() == 1 && last.exhausted && !last.nextCursor, "a full final page is terminal");

    const auto other = QCoro::waitFor(
        catalog->fetchBrowsePage(BrowseDescriptor::person(QStringLiteral("nobody")), 0, 72, {}, std::nullopt));
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

    QTemporaryDir combined;
    require(combined.isValid(), "combined-folder fixture exists");
    QDir(combined.path()).mkpath("one/nested");
    QDir(combined.path()).mkpath("two");
    for (const QString& relative :
        { QString("one/same.mp4"), QString("one/nested/child.mkv"), QString("two/same.mp4") }) {
        QFile file(combined.filePath(relative));
        require(file.open(QIODevice::WriteOnly), "folder fixture opens");
        file.write("fixture");
    }
    LocalProvider multi(
        "combined", { combined.filePath("one"), combined.filePath("two"), combined.filePath("one/nested") });
    multi.scan();
    const auto merged = QCoro::waitFor(multi.fetchLatestItems({}, 20));
    require(merged.size() == 3,
        "overlapping roots do not duplicate files; same filenames in different folders remain distinct");
    QSet<QString> identities;
    for (const auto& entry : merged)
        identities.insert(entry.id);
    require(identities.size() == 3, "combined folders cannot alias same-named media");
    bool emptyRejected = false;
    try {
        LocalProvider empty("empty", {});
    } catch (const std::runtime_error&) {
        emptyRejected = true;
    }
    require(emptyRejected, "no media directory is chosen implicitly");

    QTemporaryDir thumbnails;
    LocalProvider colored("colored", { QStringLiteral(TEST_SOURCE_DIR "/tests/fixtures") });
    colored.scan();
    const auto coloredItems = QCoro::waitFor(colored.searchItems(QStringLiteral("local-thumbnail")));
    require(coloredItems.size() == 1, "the colored video fixture is indexed");
    const auto& coloredItem = coloredItems.front();
    const QUrl thumbnail(
        colored.artwork()->imageUrl({ coloredItem.id, coloredItem.posterTag, QStringLiteral("Primary"), 300 }));
    QString thumbnailError;
    const QByteArray bytes = localThumbnail(thumbnail, thumbnails.path(), thumbnailError);
    const QImage image = QImage::fromData(bytes);
    require(!image.isNull() && image.width() <= 640 && image.height() <= 360,
        "local video produces a bounded decoded thumbnail");
    const QColor center = image.pixelColor(image.width() / 2, image.height() / 2);
    require(center.red() > 180 && center.green() < 40 && center.blue() < 40,
        "thumbnail contains the decoded red video frame, not an initial black render");
    require(localThumbnail(thumbnail, thumbnails.path(), thumbnailError) == bytes,
        "repeated thumbnail requests reuse the persisted frame");

    qputenv("SPOOL_CREDENTIAL_STORE_DIR", combined.filePath("credentials").toUtf8());
    DatabaseManager database;
    require(database.initialize(combined.filePath("accounts.sqlite")), "local account database opens");
    ProviderRegistry registry(&database);
    ProviderManifest module;
    module.id = "fixture.local";
    module.name = "Local files";
    module.version = "1.0.0";
    module.ui = { { "login", "LocalFolders.qml" }, { "settings", "LocalFolders.qml" } };
    QPointer<LocalProvider> activeLocal;
    registry.addNativeModule(
        module,
        [&](const QString& id, const QVariantMap& configuration, QObject *parent) {
            activeLocal = new LocalProvider(id, configuration.value("folders").toStringList(), parent);
            return activeLocal.data();
        },
        QUrl("qrc:/qt/qml/Spool/qml/pages/"));
    QCoro::waitFor(registry.restore());
    require(!registry.hasAccounts() && !activeLocal, "restoring a fresh profile never adds local files");
    auto *cancelled = qobject_cast<ProviderUiContext *>(registry.beginSetup(module.id));
    require(cancelled && !activeLocal, "opening folder setup neither scans nor creates an account");
    cancelled->close();
    require(!registry.hasAccounts(), "cancelling folder setup leaves the profile empty");
    auto *setup = qobject_cast<ProviderUiContext *>(registry.beginSetup(module.id));
    require(setup, "native folder setup opens without a script runtime");
    setup->complete({ { "account", "local-library" }, { "label", "Local files" },
        { "configuration",
            QVariantMap { { "folders", QStringList { combined.filePath("one"), combined.filePath("two") } } } } });
    require(
        registry.accountList().size() == 1 && activeLocal, "explicit folder confirmation activates one local account");
    const QString localAccount = registry.accountList().front().id;
    registry.setAccountEnabled(localAccount, false);
    auto *settings = qobject_cast<ProviderUiContext *>(registry.openSettings(localAccount));
    require(
        settings && settings->arguments().value("configuration").toMap().value("folders").toStringList().size() == 2,
        "disabled local accounts retain editable folder configuration");
    settings->complete(
        { { "configuration", QVariantMap { { "folders", QStringList { combined.filePath("two") } } } } });
    require(!registry.accountList().front().enabled && !registry.sourceRunning(localAccount),
        "editing a disabled local library does not silently enable it");
    registry.setAccountEnabled(localAccount, true);
    require(registry.sourceRunning(localAccount) && activeLocal, "the configured library can be enabled again");
    activeLocal->scan();
    const auto reconfigured = QCoro::waitFor(activeLocal->fetchLatestItems({}, 20));
    require(reconfigured.size() == 1 && reconfigured.front().path == combined.filePath("two/same.mp4"),
        "reenabling uses only the newly selected folder set");

    std::cout << "local provider ok\n";
    return 0;
}
