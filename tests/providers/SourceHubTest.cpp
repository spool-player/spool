#include "provider/SourceHub.h"
#include "ProviderFixture.h"
#include "TestMain.h"
#include "app/ArtworkService.h"
#include "cache/DatabaseManager.h"
#include "common/MetaJson.h"
#include "provider/PortableProvider.h"
#include "provider/ProviderRegistry.h"
#include "provider/ProviderUiContext.h"
#include "providers/local/LocalProvider.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <functional>
#include <iostream>

using namespace Spool;

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void waitUntil(const std::function<bool()>& condition, const char *message)
{
    QElapsedTimer timeout;
    timeout.start();
    while (!condition() && timeout.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(condition(), message);
}

} // namespace

// Two folders and two JS accounts behind one hub: IDs scoped per account,
// merged rows interleaved, and an account that fails left out, not fatal.
SPOOL_TEST_MAIN("source-hub")
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    qputenv("SPOOL_CREDENTIAL_STORE_DIR", directory.filePath(QStringLiteral("credentials")).toUtf8());
    DatabaseManager database;
    require(database.initialize(directory.filePath(QStringLiteral("cache.sqlite"))), "database opens");
    const QString installs = directory.filePath(QStringLiteral("providers"));
    require(ProviderPackage::install(ProviderFixture::package(), installs).has_value(), "fixture installs");

    ProviderRegistry registry(&database);
    registry.setInstallDirectory(installs);
    ProviderManifest local;
    local.id = QStringLiteral("spool.local");
    local.name = QStringLiteral("Local files");
    local.version = QStringLiteral("1.0.0");
    const QString fixtures = QStringLiteral(TEST_SOURCE_DIR "/tests/media/fixtures");
    registry.addNativeModule(local, [fixtures](const QString& id, const QVariantMap&, QObject *parent) {
        return new LocalProvider(id, fixtures, parent);
    });
    registry.loadModules();
    SourceHub hub(&registry);
    bool announced = false;
    QObject::connect(&hub, &Provider::sessionStarted, [&announced] { announced = true; });
    QCoro::waitFor(registry.restore());
    waitUntil([&] { return announced; }, "with no accounts the hub still announces itself");

    const auto add = [&](const char *module, const char *key, QVariantMap configuration = {}) {
        const QString id = registry.finishSetup({},
            { { QStringLiteral("module"), QLatin1String(module) }, { QStringLiteral("account"), QLatin1String(key) },
                { QStringLiteral("label"), QString::fromLatin1(key).toUpper() },
                { QStringLiteral("configuration"), configuration } });
        registry.useAccount(id);
        return id;
    };
    const QString a = add("spool.local", "a");
    const QString b = add("spool.local", "b");
    const QString remote = add("fixture.test", "remote",
        { { QStringLiteral("label"), QStringLiteral("Remote") }, { QStringLiteral("inheritedArtwork"), true } });
    const QString offline = add("fixture.test", "offline", { { QStringLiteral("failing"), true } });
    waitUntil([&] { return hub.sources().size() == 4; }, "every enabled account joins the hub");
    require(hub.capabilities().testFlag(Provider::Search) && hub.capabilities().testFlag(Provider::UserItemState),
        "the hub offers what any of its accounts can do");
    waitUntil([&] { return QCoro::waitFor(hub.fetchLatestItems({}, 100)).size() == 11; },
        "latest rows merge every account that answers");

    const auto libraries = QCoro::waitFor(hub.fetchLibraries());
    require(libraries.size() == 3, "libraries from every account that answers");
    QStringList names;
    for (const LibraryItem& library : libraries)
        names.append(library.name);
    names.sort();
    require(names
            == QStringList({ QStringLiteral("Shelf"), QStringLiteral("fixtures · A"), QStringLiteral("fixtures · B") }),
        "same-named libraries say whose they are");
    for (const LibraryItem& library : libraries) {
        require(SourceHub::rawId(library.id) == QStringLiteral("local")
                || SourceHub::rawId(library.id) == QStringLiteral("lib"),
            "scoping keeps the source's own id");
        require(!hub.accountOf(library.id).isEmpty(), "every scoped id finds its account");
    }
    require(hub.accountOf(QStringLiteral("unscoped")).isEmpty()
            && SourceHub::rawId(QStringLiteral("unscoped")) == QStringLiteral("unscoped"),
        "unscoped ids belong to nobody");

    const auto latest = QCoro::waitFor(hub.fetchLatestItems({}, 6));
    require(latest.size() == 6, "a merged row honours its limit");
    QSet<QString> firstThree;
    for (int i = 0; i < 3; ++i)
        firstThree.insert(hub.accountOf(latest[i].id));
    require(firstThree.size() == 3, "merged rows interleave accounts instead of listing one after another");
    require(!firstThree.contains(offline), "a failing account is left out");

    const MovieItem details = QCoro::waitFor(hub.fetchItemDetails(latest[0].id));
    require(details.id == latest[0].id, "details round-trip the scoped id");
    const QString remoteItem = hub.scoped(remote, QStringLiteral("m1"));
    const auto byIds = QCoro::waitFor(
        hub.fetchItemsByIds({ remoteItem, latest[1].id, hub.scoped(offline, QStringLiteral("gone")), latest[0].id }));
    require(
        byIds.size() == 3 && byIds[0].id == remoteItem && byIds[1].id == latest[1].id && byIds[2].id == latest[0].id,
        "lookups across accounts keep the order asked for");

    require(QCoro::waitFor(hub.fetchSimilarItems(remoteItem)).empty()
            && QCoro::waitFor(hub.fetchLibraryFilterOptions(hub.scoped(remote, QStringLiteral("lib")))).isEmpty(),
        "operations a provider leaves out answer empty rather than failing");

    ArtworkSource::ImageRequest image;
    image.itemId = remoteItem;
    image.tag = QStringLiteral("t");
    image.imageType = QStringLiteral("Primary");
    image.maxWidth = 300;
    require(hub.imageUrl(image) == QStringLiteral("https://img.invalid/m1/Primary?w=300"),
        "artwork is built from the owning account's template with its own id");

    // Exercise provider decoding, account scoping, cache persistence and the
    // actual home/details artwork selector together: parent tags need parent IDs.
    const MovieItem inherited = QCoro::waitFor(hub.fetchItemDetails(remoteItem));
    MovieItem cached = metaFromJson<MovieItem>(metaToJson(inherited));
    ArtworkService artwork(QString(), 0, 1024, 1, nullptr);
    artwork.setSource(&hub);
    require(artwork.url(QVariant::fromValue(cached), QStringLiteral("landscape"), 400)
            == QStringLiteral("https://img.invalid/parent-thumb/Thumb?w=400"),
        "cached home cards request inherited thumbnails from their account-scoped owner");
    require(artwork.url(QVariant::fromValue(cached), QStringLiteral("backdrop"), 1920)
            == QStringLiteral("https://img.invalid/parent-backdrop/Backdrop?w=1920"),
        "details request inherited backdrops from their own owner, not the thumbnail owner");
    cached.thumbTag.clear();
    require(artwork.url(QVariant::fromValue(cached), QStringLiteral("landscape"), 400)
            == QStringLiteral("https://img.invalid/parent-backdrop/Backdrop?w=400"),
        "a home card without a thumbnail falls back to its backdrop owner");
    cached.thumbTag = QStringLiteral("own-thumb");
    cached.thumbItemId.clear();
    cached.albumId = hub.scoped(remote, QStringLiteral("album"));
    cached.albumPrimaryImageTag = QStringLiteral("album-cover");
    require(artwork.url(QVariant::fromValue(cached), QStringLiteral("landscape"), 400)
            == QStringLiteral("https://img.invalid/m1/Thumb?w=400"),
        "an item's own thumbnail overrides both inherited and album cover identities");

    const auto menuActions = [&](const QString& itemId, const QString& type) {
        QVariantList actions;
        int completed = -1;
        const auto connection = QObject::connect(
            &hub, &SourceHub::itemActionsReady, [&](int request, const QVariantList& result, const QString&) {
                completed = request;
                actions = result;
            });
        const int request = hub.requestItemActions(itemId, type);
        waitUntil([&] { return completed == request; }, "menu policy completes");
        QObject::disconnect(connection);
        return actions;
    };
    require(menuActions(remoteItem, QStringLiteral("Movie")).size() == 1
            && menuActions(remoteItem, QStringLiteral("Series")).isEmpty()
            && menuActions(hub.scoped(a, QStringLiteral("x")), QStringLiteral("Movie")).isEmpty(),
        "older providers retain type-filtered manifest actions through the asynchronous menu");

    QString changed;
    QString toast;
    QObject::connect(&hub, &Provider::contentChanged, [&changed](const QString& id) { changed = id; });
    QObject::connect(&hub, &Provider::toastRequested, [&toast](const QString& message) { toast = message; });
    QObject::connect(&registry, &ProviderRegistry::componentRequested, [](QObject *context) {
        qobject_cast<ProviderUiContext *>(context)->complete({ { QStringLiteral("name"), QStringLiteral("Weekend") } });
    });
    hub.runItemAction(QStringLiteral("tag"), remoteItem, QStringLiteral("Movie"));
    waitUntil([&] { return toast == QStringLiteral("tag:Weekend"); }, "an action can ask the viewer before it runs");
    require(changed == remoteItem, "the change it reports is scoped back");
    changed.clear();
    QCoro::waitFor(
        hub.call(remote, QStringLiteral("announce"), { { QStringLiteral("itemId"), QStringLiteral("m2") } }));
    waitUntil([&] { return changed == hub.scoped(remote, QStringLiteral("m2")); }, "pushed changes are scoped too");

    registry.setAccountEnabled(b, false);
    require(hub.sources().size() == 3 && !hub.source(b), "a disabled account leaves the hub");
    const auto remaining = QCoro::waitFor(hub.fetchLibraries());
    require(remaining.size() == 2, "and its libraries go with it");
    for (const LibraryItem& library : remaining)
        require(!library.name.contains(QStringLiteral(" · ")), "a name no longer shared is shown plain");
    require(hub.accountOf(hub.scoped(a, QStringLiteral("x"))) == a, "the others keep their scope");

    // Several users of one server: search goes through as few of them as
    // reach every library, and never shows the same item twice.
    const auto user = [&](const char *key, const char *server, QStringList libraries, bool exact = false) {
        const QString id = registry.finishSetup({},
            { { QStringLiteral("module"), QStringLiteral("fixture.test") },
                { QStringLiteral("account"), QLatin1String(key) }, { QStringLiteral("group"), QLatin1String(server) },
                { QStringLiteral("label"), QLatin1String(key) },
                { QStringLiteral("configuration"),
                    QVariantMap { { QStringLiteral("label"), QLatin1String(key) },
                        { QStringLiteral("libraries"), libraries }, { QStringLiteral("exact"), exact } } } });
        registry.useAccount(id);
        waitUntil([&] { return registry.sourceRunning(id); }, "the selected user commits before another login");
        return id;
    };
    // The account used last on each server is the one in use.
    const QString wide = user("wide", "s1", { QStringLiteral("m"), QStringLiteral("anime") });
    const QString same = user("same", "s1", { QStringLiteral("m") });
    const QString narrow = user("narrow", "s1", { QStringLiteral("m") });
    const QString twin = user("twin", "s2", { QStringLiteral("m") });
    const QString used = user("used", "s2", { QStringLiteral("m") });
    const QString other = user("other", "s3", { QStringLiteral("m"), QStringLiteral("anime") });
    const QString mine = user("mine", "s3", { QStringLiteral("m"), QStringLiteral("k") }, true);
    waitUntil([&] { return hub.source(narrow) && hub.source(used) && hub.source(mine); }, "the users in use start");
    const size_t browsed = hub.sources().size();
    const QString scopeKey = hub.libraryScopeKey();
    hub.prepareSearch();
    waitUntil([&] { return hub.source(wide) && hub.source(same) && hub.source(twin) && hub.source(other); },
        "users set aside start for search");
    require(hub.sources().size() == browsed && hub.libraryScopeKey() == scopeKey,
        "and stay out of browsing and the library caches");

    QStringList planned;
    for (const SourceHub::SearchTarget& target : QCoro::waitFor(hub.searchPlan()))
        planned.append(target.accountId);
    require(planned.contains(wide) && !planned.contains(narrow) && !planned.contains(same),
        "a user who sees more stands in for those who see less");
    require(planned.contains(used) && !planned.contains(twin), "of two who see the same, the one in use searches");
    require(planned.indexOf(mine) >= 0 && planned.indexOf(mine) < planned.indexOf(other),
        "overlapping users both search, the one in use first");

    int updates = 0;
    std::vector<MovieItem> found;
    QCoro::waitFor(hub.searchProgressively(QStringLiteral("film"), 80, [&](std::vector<MovieItem> items) {
        ++updates;
        found = std::move(items);
    }));
    require(updates >= 2, "results arrive as each account answers");
    QStringList s3;
    for (const MovieItem& item : found) {
        if (hub.accountOf(item.id) == mine || hub.accountOf(item.id) == other)
            s3.append(SourceHub::rawId(item.id));
    }
    s3.sort();
    require(s3
            == QStringList(
                { QStringLiteral("anime-1"), QStringLiteral("exact"), QStringLiteral("k-1"), QStringLiteral("m-1") }),
        "an item two users of a server can both see is listed once");
    require(!found.empty() && found.front().title == QStringLiteral("The Film"), "the exact title ranks first");
    const MovieItem shared = *std::find_if(found.begin(), found.end(), [&](const MovieItem& item) {
        return SourceHub::rawId(item.id) == QStringLiteral("m-1") && hub.accountOf(item.id) != wide
            && hub.accountOf(item.id) != used;
    });
    require(hub.accountOf(shared.id) == mine, "and it comes from the user in use");

    registry.useAccount(wide);
    waitUntil([&] { return !hub.source(narrow); }, "choosing another user sets the last one aside");
    const auto browsing = hub.sources();
    require(std::find(browsing.begin(), browsing.end(), hub.source(wide)) != browsing.end(),
        "an account running for search is promoted when chosen");
    const QString pagingAccount = add("fixture.test", "pagination", { { QStringLiteral("pagination"), true } });
    waitUntil([&] { return hub.source(pagingAccount) != nullptr; }, "pagination fixture starts");
    const auto scoped = [&](const char *id) { return hub.scoped(pagingAccount, QString::fromLatin1(id)); };
    auto *portable = qobject_cast<PortableProvider *>(hub.source(pagingAccount));
    require(portable != nullptr, "fixture exposes the portable catalogue");
    const auto sparse = QCoro::waitFor(hub.fetchEpisodes(scoped("sparse")));
    require(sparse.size() == 3 && sparse[0].id == scoped("first") && sparse[1].id == scoped("second")
            && sparse[2].id == scoped("last"),
        "collectors traverse short and empty pages with opaque advancing cursors");
    require(sparse[0].playlistItemId == QStringLiteral("entry:first"), "occurrence IDs remain opaque and unscoped");
    const auto descriptor = BrowseDescriptor::library(scoped("sparse"), QStringLiteral("movies"));
    const auto first = QCoro::waitFor(hub.fetchBrowsePage(descriptor, 0, 100, {}, std::nullopt));
    const auto empty = QCoro::waitFor(hub.fetchBrowsePage(descriptor, 2, 100, {}, first.nextCursor));
    const auto final = QCoro::waitFor(hub.fetchBrowsePage(descriptor, 2, 100, {}, empty.nextCursor));
    require(first.items.size() == 2 && first.nextCursor == QStringLiteral("s:1") && !first.exhausted
            && empty.items.empty() && empty.nextCursor == QStringLiteral("s:2") && !empty.exhausted
            && final.items.size() == 1 && final.exhausted,
        "browse forwards opaque continuation independently of the UI offset");
    require(QCoro::waitFor(hub.fetchSeasons(scoped("many"))).size() == 205,
        "seasons are collected beyond the first hundred rows");
    const auto credits = QCoro::waitFor(hub.fetchItemsByPerson(scoped("many"), 150));
    require(credits.items.size() == 150 && credits.items.back().id == scoped("149"),
        "person credits honor their requested maximum across pages");
    require(QCoro::waitFor(portable->fetchResumeItems(150)).size() == 150
            && QCoro::waitFor(portable->fetchNextUpEpisodes(150)).size() == 150,
        "requested-count lists fill their limit across provider pages");
    const auto rejects = [&](QCoro::Task<std::vector<MovieItem>> task, const char *code) {
        try {
            QCoro::waitFor(std::move(task));
        } catch (const std::exception& error) {
            return QByteArray(error.what()) == code;
        }
        return false;
    };
    require(rejects(portable->fetchEpisodes(QStringLiteral("repeat")), "invalid_pagination"),
        "repeated cursors fail rather than looping");
    require(rejects(portable->fetchEpisodes(QStringLiteral("missing")), "invalid_pagination"),
        "nonterminal pages require continuation");
    require(rejects(portable->fetchEpisodes(QStringLiteral("empty-forever")), "response_limit"),
        "collect-all has a hard page ceiling even for advancing empty pages");
    try {
        QCoro::waitFor(portable->fetchItemsByPerson(QStringLiteral("empty-forever"), 1));
        require(false, "requested-count collectors must enforce the page ceiling");
    } catch (const std::exception& error) {
        require(QByteArray(error.what()) == "response_limit", "requested-count page ceiling reports response_limit");
    }
    require(rejects(portable->fetchEpisodes(QStringLiteral("too-many")), "response_limit"),
        "collect-all refuses false completion at ten thousand rows");

    QStringList ids;
    for (int i = 0; i < 101; ++i)
        ids.append(hub.scoped(pagingAccount, QString::number(i)));
    ids.insert(1, ids.at(17));
    ids.append(ids.at(100));
    ids.append(scoped("missing"));
    const auto fetched = QCoro::waitFor(hub.fetchItemsByIds(ids));
    QStringList ordered;
    for (const MovieItem& row : fetched)
        ordered.append(row.id);
    ids.removeLast();
    require(ordered == ids, "ID lookup reconstructs every duplicate occurrence in input order and omits missing IDs");
    const QVariantMap stats = QCoro::waitFor(hub.call(pagingAccount, QStringLiteral("batchStats")));
    const QVariantList requests = stats.value(QStringLiteral("requests")).toList();
    QSet<QString> requested;
    require(requests.size() == 3, "one hundred and two unique IDs use three bounded batches");
    for (const QVariant& request : requests) {
        const QVariantList batch = request.toList();
        require(batch.size() <= 50, "each metadata request contains at most fifty IDs");
        for (const QVariant& id : batch) {
            require(!requested.contains(id.toString()), "each unique ID is fetched only once");
            requested.insert(id.toString());
        }
    }
    require(requested.size() == 102, "batch records contain every unique requested ID including missing metadata");
    require(stats.value(QStringLiteral("maximumActive")).toInt() <= 2, "metadata batches have bounded concurrency");
    QStringList rawIds;
    for (int i = 0; i < 101; ++i)
        rawIds.append(QString::number(i));
    auto lookupA = portable->fetchItemsByIds(rawIds);
    auto lookupB = portable->fetchItemsByIds(rawIds);
    require(QCoro::waitFor(std::move(lookupA)).size() == 101 && QCoro::waitFor(std::move(lookupB)).size() == 101,
        "concurrent callers both complete their metadata lookups");
    require(QCoro::waitFor(hub.call(pagingAccount, QStringLiteral("batchStats")))
                .value(QStringLiteral("maximumActive"))
                .toInt()
            == 2,
        "two overlapping callers still issue at most two batches per account");
    for (const QString& operation : { QStringLiteral("groupSend"), QStringLiteral("remoteCommand") }) {
        QVariantMap command { { QStringLiteral("itemIds"), QStringList { scoped("first"), remoteItem } },
            { QStringLiteral("action"), QStringLiteral("setQueue") } };
        const QVariantMap arguments = operation == QStringLiteral("remoteCommand")
            ? QVariantMap { { QStringLiteral("command"), command } }
            : command;
        try {
            QCoro::waitFor(hub.call(pagingAccount, operation, arguments));
            require(false, "mixed-account server queues must fail");
        } catch (const std::exception& error) {
            require(QByteArray(error.what()) == "mixed_source_queue", "mixed queues have a distinct error");
        }
    }
    require(QCoro::waitFor(hub.call(pagingAccount, QStringLiteral("batchStats")))
                .value(QStringLiteral("queueCalls"))
                .toInt()
            == 0,
        "mixed queues fail before invoking provider code");
    QCoro::waitFor(hub.call(pagingAccount, QStringLiteral("groupSend"),
        { { QStringLiteral("itemIds"), QStringList { scoped("first"), scoped("first") } } }));
    const QVariantMap queue = QCoro::waitFor(hub.call(pagingAccount, QStringLiteral("batchStats")));
    require(queue.value(QStringLiteral("queue")).toStringList()
            == QStringList { QStringLiteral("first"), QStringLiteral("first") },
        "valid server queues store raw IDs while retaining duplicate occurrences");
    require(QCoro::waitFor(portable->fetchSearchSuggestions()).empty(),
        "an account without suggestions never substitutes its populated resume results");
    const QString reporting = add("fixture.test", "reporting",
        { { QStringLiteral("catalogueExtensions"), true }, { QStringLiteral("label"), QStringLiteral("Reporting") } });
    waitUntil([&] { return hub.source(reporting) != nullptr; }, "reporting account starts");
    const auto suggestions = QCoro::waitFor(hub.source(reporting)->search()->fetchSearchSuggestions());
    require(suggestions.size() == 1 && suggestions.front().id == QStringLiteral("suggestion"),
        "negotiated suggestions invoke the dedicated operation");
    const QString song = hub.scoped(reporting, QStringLiteral("song"));
    std::vector<SourceHub::ReportingQueueEntry> reportingItems { { song, QStringLiteral("opaque:first"), true },
        { remoteItem, {}, false }, { song, QStringLiteral("opaque:second"), true },
        { hub.scoped(reporting, QStringLiteral("film")), {}, false } };
    hub.setPlaybackQueue(reportingItems, 2);
    PlaybackSession session;
    session.itemId = song;
    session.playSessionId = QStringLiteral("unchanged-session");
    const auto reports = [&]() {
        return QCoro::waitFor(hub.call(reporting, QStringLiteral("reportStats")))
            .value(QStringLiteral("reports"))
            .toList();
    };
    const auto progress
        = [&]() { QCoro::waitFor(hub.playback()->reportPlaybackProgress(session, 100, false, 1, 100, false)); };
    QCoro::waitFor(hub.playback()->reportPlaybackStart(session, 1, 100, false));
    const QVariantMap started = reports().last().toMap();
    const QVariantMap initialQueue = started.value(QStringLiteral("queue")).toMap();
    const QVariantList rows = initialQueue.value(QStringLiteral("items")).toList();
    require(rows.size() == 3 && rows.at(0).toMap().value("itemId").toString() == "song"
            && rows.at(1).toMap().value("itemId").toString() == "song"
            && rows.at(0).toMap().value("entryId").toString() == "opaque:first"
            && rows.at(1).toMap().value("entryId").toString() == "opaque:second"
            && rows.at(0).toMap().value("mediaType").toString() == "audio"
            && rows.at(2).toMap().value("mediaType").toString() == "video" && started.value("queueIndex").toInt() == 1,
        "reports filter foreign accounts, preserve duplicate occurrences and adjust the current index");
    hub.setPlaybackQueue(reportingItems, 3);
    progress();
    require(!reports().last().toMap().contains("queue") && reports().last().toMap().value("queueIndex").toInt() == 2,
        "index-only changes do not resend the immutable queue snapshot");
    QCoro::waitFor(hub.playback()->reportPlaybackStart(session, 1, 100, false));
    require(reports().last().toMap().value("queue").toMap() == initialQueue,
        "every playback start includes the current snapshot even when membership is unchanged");
    std::swap(reportingItems[0], reportingItems[2]);
    hub.setPlaybackQueue(reportingItems, 0);
    progress();
    const QVariantMap reordered = reports().last().toMap().value("queue").toMap();
    require(reordered.value("revision") != initialQueue.value("revision")
            && reordered.value("items").toList().first().toMap().value("entryId").toString() == "opaque:second",
        "queue edits reach an unchanged playback session as a new occurrence-ordered revision");
    hub.setPlaybackQueue(reportingItems, -1);
    progress();
    require(!reports().last().toMap().contains("queue") && !reports().last().toMap().contains("queueIndex"),
        "unknown shuffled duplicate occurrence indexes are omitted rather than guessed");
    QCoro::waitFor(hub.playback()->reportPlaybackStopped(session, 100, false, 1));
    require(!reports().last().toMap().contains("queue") && !reports().last().toMap().contains("queueIndex"),
        "stop reports retain their existing queue-free contract");
    registry.setAccountEnabled(reporting, false);
    registry.setAccountEnabled(reporting, true);
    waitUntil([&] { return hub.source(reporting) != nullptr; }, "reporting source restarts");
    progress();
    require(reports().last().toMap().value("queue").toMap() == reordered,
        "a restarted provider receives the cached queue even before another start or membership change");
    int queueStatuses = 0;
    int playbackErrors = 0;
    QObject::connect(&hub, &Provider::errorOccurred, [&] { ++playbackErrors; });
    QObject::connect(&hub, &SourceHub::accountEvent, [&](const QString&, const QString& type, const QVariantMap&) {
        if (type == QStringLiteral("playbackQueueStatus"))
            ++queueStatuses;
    });
    QCoro::waitFor(hub.call(reporting, QStringLiteral("queueStatus"),
        { { "revision", QStringLiteral("stale") }, { "state", QStringLiteral("unavailable") } }));
    QCoro::waitFor(hub.call(reporting, QStringLiteral("queueStatus"),
        { { "revision", reordered.value("revision") }, { "state", QStringLiteral("unavailable") } }));
    waitUntil([&] { return queueStatuses == 1; }, "only the current queue revision exposes nonfatal status");
    require(playbackErrors == 0, "optional queue limitations never become playback failures");
    reportingItems.erase(reportingItems.begin());
    hub.setPlaybackQueue(reportingItems, 1);
    progress();
    const QVariantList afterRemoval = reports().last().toMap().value("queue").toMap().value("items").toList();
    require(afterRemoval.size() == 2 && afterRemoval.first().toMap().value("entryId").toString() == "opaque:first",
        "removing the second occurrence keeps the first occurrence of the same media ID");
    hub.setPlaybackQueue({}, -1);
    progress();
    require(reports().last().toMap().contains("queue")
            && reports().last().toMap().value("queue").toMap().value("items").toList().isEmpty(),
        "empty membership is reported as an explicit new revision, not stale cached entries");
    PlaybackSession baselineSession;
    baselineSession.itemId = remoteItem;
    QCoro::waitFor(hub.playback()->reportPlaybackStart(baselineSession, 1, 100, false));
    const QVariantMap baselineReport
        = QCoro::waitFor(hub.call(remote, QStringLiteral("reportStats"))).value("reports").toList().last().toMap();
    require(!baselineReport.contains("queue") && !baselineReport.contains("queueIndex"),
        "non-negotiated accounts retain baseline reports without extension fields");
    return 0;
}
