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

SPOOL_TEST_MAIN("emby-provider-compatibility")
{
    QCoreApplication app(argc, argv);
    const QString origin = qEnvironmentVariable("SPOOL_TEST_EMBY_ORIGIN");
    if (origin.isEmpty())
        return 77;
    const QUrl url(origin, QUrl::StrictMode);
    require(url.scheme() == QLatin1String("http") && url.authority().startsWith(QLatin1String("127.0.0.1:"))
            && url.port() > 0 && url.path().isEmpty() && !url.hasQuery() && !url.hasFragment(),
        "Emby compatibility requires a private loopback server");

    QTemporaryDir directory;
    require(directory.isValid(), "create a private Emby profile");
    qputenv("SPOOL_CREDENTIAL_STORE_DIR", directory.filePath(QStringLiteral("credentials")).toUtf8());
    DatabaseManager database;
    require(database.initialize(directory.filePath(QStringLiteral("cache.sqlite"))), "open the private database");
    ProviderRegistry registry(&database);
    registry.setInstallDirectory(directory.filePath(QStringLiteral("providers")));
    registry.setRuntimeEnvironment(
        { { "id", "spool-emby-client" }, { "name", "Spool compatibility" }, { "app", "Spool" }, { "version", "0.0.0" },
            { "platform", "linux" }, { "locale", "en" } },
        {});
    registry.loadModules();
    SourceHub hub(&registry);
    QCoro::waitFor(registry.restore());

    auto *setup = qobject_cast<ProviderUiContext *>(registry.beginSetup(QStringLiteral("spool.emby")));
    require(setup, "open the bundled Emby setup context");
    QCoro::waitFor(registry.allowSetupOrigin(setup->sourceId(), url));
    const auto probe
        = QCoro::waitFor(registry.callSource(setup->sourceId(), QStringLiteral("probe"), { { "server", origin } }));
    require(!probe.value("id").toString().isEmpty()
            && probe.value("version") == qEnvironmentVariable("SPOOL_TEST_EMBY_VERSION"),
        "discover the fixture server and verify its reported version");
    const auto authenticate = [&](bool valid) {
        return QCoro::waitFor(registry.callSource(setup->sourceId(), QStringLiteral("authenticate"),
            { { "server", origin }, { "username", "spool-fixture" },
                { "password", valid ? "spool-fixture-password" : "invalid-test-password" } }));
    };
    bool rejected = false;
    try {
        authenticate(false);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected && registry.accountList().empty(), "reject invalid local credentials without adding an account");

    QString accountId;
    QObject::connect(&registry, &ProviderRegistry::accountAdded, &registry, [&](const QString& id) { accountId = id; });
    setup->complete(authenticate(true));
    QElapsedTimer timeout;
    timeout.start();
    while ((!hub.source(accountId) || !hub.source(accountId)->ready()) && timeout.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    require(!accountId.isEmpty() && hub.source(accountId) && hub.source(accountId)->ready(),
        "activate the local Emby identity");

    const auto libraries = QCoro::waitFor(hub.fetchLibraries());
    require(libraries.size() == 1, "list the synthetic movie library");
    const auto page = QCoro::waitFor(hub.fetchBrowsePage(
        BrowseDescriptor::library(libraries.front().id, libraries.front().collectionType), 0, 10, {}, std::nullopt));
    require(
        page.items.size() == 1 && hub.accountOf(page.items.front().id) == accountId, "browse one scoped library item");
    const auto item = QCoro::waitFor(hub.fetchItemDetails(page.items.front().id));
    require(item.id == page.items.front().id && item.title == QLatin1String("Synthetic Film"),
        "load the synthetic movie details");
    const auto results = QCoro::waitFor(hub.searchItems(QStringLiteral("Synthetic")));
    require(!results.empty() && results.front().id == item.id, "search returns the same scoped movie");
    require(hub.capabilities().testFlag(Provider::UserItemState)
            && hub.capabilities().testFlag(Provider::PlaybackReporting)
            && !hub.capabilities().testFlag(Provider::GroupPlayback),
        "the running server offers state and reporting and gates group playback");
    QCoro::waitFor(hub.setItemFavorite(item.id, true));
    require(QCoro::waitFor(hub.fetchItemDetails(item.id)).favorite, "the server persists a favorite mutation");
    QCoro::waitFor(hub.setItemFavorite(item.id, false));
    require(!QCoro::waitFor(hub.fetchItemDetails(item.id)).favorite, "the server persists favorite removal");

    auto *provider = hub.playback();
    const auto playback = QCoro::waitFor(provider->resolvePlayback(item, false));
    require(playback.itemId == item.id && playback.playMethod == QLatin1String("DirectPlay"),
        "resolve direct playback for the scoped item");
    require(QUrl(playback.url).adjusted(QUrl::RemovePath | QUrl::RemoveQuery) == url
            && !provider->mediaRequestHeaders().isEmpty(),
        "deliver authenticated media from the synthetic server origin");
    QCoro::waitFor(provider->reportPlaybackStart(playback, 1.0, 100, false));
    ProviderCompatibility::verifyDecodedVideo(playback, provider->mediaRequestHeaders());
    QCoro::waitFor(provider->reportPlaybackStopped(playback, 30'000'000, false, 1.0));
    return 0;
}
