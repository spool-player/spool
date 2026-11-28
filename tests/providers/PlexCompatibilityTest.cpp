#include "ProviderCompatibilityVideo.h"
#include "TestMain.h"
#include "TestRequire.h"
#include "cache/DatabaseManager.h"
#include "provider/PlaybackSource.h"
#include "provider/ProviderRegistry.h"
#include "provider/ProviderUiContext.h"
#include "provider/SourceHub.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

using namespace Spool;
using SpoolTests::require;

SPOOL_TEST_MAIN("plex-provider-compatibility")
{
    QCoreApplication app(argc, argv);
    const QString origin = qEnvironmentVariable("SPOOL_TEST_PLEX_ORIGIN");
    if (origin.isEmpty())
        return 77;
    const QUrl url(origin, QUrl::StrictMode);
    require(url.scheme() == QLatin1String("http") && url.authority().startsWith(QLatin1String("127.0.0.1:"))
            && url.port() > 0 && url.path().isEmpty() && !url.hasQuery() && !url.hasFragment(),
        "Plex compatibility requires an isolated loopback server");

    QTemporaryDir directory;
    require(directory.isValid(), "create a private Plex profile");
    qputenv("SPOOL_CREDENTIAL_STORE_DIR", directory.filePath(QStringLiteral("credentials")).toUtf8());
    DatabaseManager database;
    require(database.initialize(directory.filePath(QStringLiteral("cache.sqlite"))), "open the private database");
    ProviderRegistry registry(&database);
    registry.setInstallDirectory(directory.filePath(QStringLiteral("providers")));
    registry.setRuntimeEnvironment({ { "id", "spool-plex-test" }, { "name", "Spool compatibility" }, { "app", "Spool" },
                                       { "version", "0.0.0" }, { "platform", "linux" }, { "locale", "en" } },
        {});
    registry.loadModules();
    SourceHub hub(&registry);
    QCoro::waitFor(registry.restore());

    auto *setup = qobject_cast<ProviderUiContext *>(registry.beginSetup(QStringLiteral("spool.plex")));
    require(setup, "open the bundled Plex setup context");
    QCoro::waitFor(registry.allowSetupOrigin(setup->sourceId(), url));
    const auto identity
        = QCoro::waitFor(registry.callSource(setup->sourceId(), QStringLiteral("identify"), { { "address", origin } }));
    const auto serverId = identity.value("id").toString();
    require(!serverId.isEmpty() && identity.value("address") == origin, "identify the exact local PMS origin");

    // The cloud linking UI is outside this fixture. Complete the saved PMS-only
    // profile shape using a synthetic marker for an unclaimed local server.
    const QVariantMap configuration { { "server", origin }, { "serverId", serverId },
        { "token", QStringLiteral("fixture-token") }, { "userId", QStringLiteral("fixture-user") },
        { "serverName", QStringLiteral("Fixture PMS") }, { "userName", QStringLiteral("Fixture viewer") },
        { "owned", true } };
    QString accountId;
    QObject::connect(&registry, &ProviderRegistry::accountAdded, &registry, [&](const QString& id) { accountId = id; });
    setup->complete({ { "account", QStringLiteral("fixture-user@") + serverId }, { "group", serverId },
        { "label", QStringLiteral("Fixture viewer") }, { "configuration", configuration } });
    QElapsedTimer timeout;
    timeout.start();
    while ((!hub.source(accountId) || !hub.source(accountId)->ready()) && timeout.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    require(!accountId.isEmpty() && hub.source(accountId) && hub.source(accountId)->ready(),
        "open the local PMS-only source");

    const auto libraries = QCoro::waitFor(hub.fetchLibraries());
    require(libraries.size() == 1, "list the synthetic local library");
    const auto page = QCoro::waitFor(hub.fetchBrowsePage(
        BrowseDescriptor::library(libraries.front().id, libraries.front().collectionType), 0, 10, {}, std::nullopt));
    require(
        page.items.size() == 1 && hub.accountOf(page.items.front().id) == accountId, "browse one scoped local movie");
    const auto item = QCoro::waitFor(hub.fetchItemDetails(page.items.front().id));
    require(item.id == page.items.front().id && item.title.startsWith(QLatin1String("Synthetic Film")),
        "load the same movie details");
    bool rejected = false;
    try {
        QCoro::waitFor(hub.fetchItemDetails(hub.scoped(accountId, QStringLiteral("9999999"))));
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "surface a missing item from the local server");
    const auto results = QCoro::waitFor(hub.searchItems(QStringLiteral("Synthetic")));
    require(!results.empty() && results.front().id == item.id, "search finds the same local movie");
    require(hub.capabilities().testFlag(Provider::UserItemState)
            && hub.capabilities().testFlag(Provider::PlaybackReporting)
            && !hub.capabilities().testFlag(Provider::GroupPlayback),
        "the PMS-only source offers state and reporting without group playback");
    QCoro::waitFor(hub.setItemFavorite(item.id, true));
    require(QCoro::waitFor(hub.fetchItemDetails(item.id)).favorite, "persist the running profile's favorite state");
    QCoro::waitFor(hub.setItemFavorite(item.id, false));
    require(!QCoro::waitFor(hub.fetchItemDetails(item.id)).favorite, "remove the favorite on the same server");

    auto *provider = hub.playback();
    const auto playback = QCoro::waitFor(provider->resolvePlayback(item, false));
    require(playback.itemId == item.id && playback.playMethod == QLatin1String("DirectPlay"),
        "resolve direct playback for the scoped local movie");
    require(QUrl(playback.url).adjusted(QUrl::RemovePath | QUrl::RemoveQuery) == url
            && !provider->mediaRequestHeaders().isEmpty(),
        "return the local part and use the app's media headers");
    QCoro::waitFor(provider->reportPlaybackStart(playback, 1.0, 100, false));
    ProviderCompatibility::verifyDecodedVideo(playback, provider->mediaRequestHeaders());
    QCoro::waitFor(provider->reportPlaybackStopped(playback, 30'000'000, false, 1.0));
    return 0;
}
