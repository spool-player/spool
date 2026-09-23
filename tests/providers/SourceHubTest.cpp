#include "provider/SourceHub.h"
#include "ProviderFixture.h"
#include "TestMain.h"
#include "cache/DatabaseManager.h"
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

using namespace JellyfinNative;

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
JELLYFIN_TEST_MAIN("source-hub")
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    qputenv("JELLYFIN_CREDENTIAL_STORE_DIR", directory.filePath(QStringLiteral("credentials")).toUtf8());
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
    const QString remote = add("fixture.test", "remote", { { QStringLiteral("label"), QStringLiteral("Remote") } });
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

    require(hub.itemActions(remoteItem, QStringLiteral("Movie")).size() == 1
            && hub.itemActions(remoteItem, QStringLiteral("Series")).isEmpty()
            && hub.itemActions(hub.scoped(a, QStringLiteral("x")), QStringLiteral("Movie")).isEmpty(),
        "item actions come from the owning provider's manifest, by type");

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
    return 0;
}
