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

SPOOL_TEST_MAIN("stremio-addon-provider")
{
    QCoreApplication app(argc, argv);
    const QString origin = qEnvironmentVariable("SPOOL_TEST_STREMIO_ORIGIN");
    if (origin.isEmpty())
        return 77;
    const QUrl fixture(origin, QUrl::StrictMode);
    require(fixture.scheme() == QLatin1String("http") && fixture.host() == QLatin1String("127.0.0.1")
            && fixture.port() > 0 && fixture.path().isEmpty()
            && fixture.authority().startsWith(QLatin1String("127.0.0.1:")) && !fixture.hasQuery()
            && !fixture.hasFragment(),
        "Stremio setup must use the test's local add-on");

    QTemporaryDir directory;
    require(directory.isValid(), "create a private Stremio profile");
    qputenv("SPOOL_CREDENTIAL_STORE_DIR", directory.filePath(QStringLiteral("credentials")).toUtf8());
    DatabaseManager database;
    require(database.initialize(directory.filePath(QStringLiteral("cache.sqlite"))), "open the private database");
    ProviderRegistry registry(&database);
    registry.setInstallDirectory(directory.filePath(QStringLiteral("providers")));
    registry.setRuntimeEnvironment({ { "id", "spool-stremio-test" }, { "name", "Spool test" }, { "app", "Spool" },
                                       { "version", "0.0.0" }, { "platform", "linux" }, { "locale", "en" } },
        {});
    registry.loadModules();
    SourceHub hub(&registry);
    QCoro::waitFor(registry.restore());

    auto *setup = qobject_cast<ProviderUiContext *>(registry.beginSetup(QStringLiteral("spool.stremio")));
    require(setup, "open the bundled add-on setup context");
    QCoro::waitFor(registry.allowSetupOrigin(setup->sourceId(), fixture));
    const auto setupWith = [&](QString path) {
        return QVariantMap { { "addons", QStringList { origin + path } }, { "imageOrigins", QVariantList {} } };
    };
    bool rejected = false;
    try {
        QCoro::waitFor(registry.callSource(
            setup->sourceId(), QStringLiteral("configure"), setupWith(QStringLiteral("/missing/manifest.json"))));
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected && registry.accountList().empty(), "a missing add-on manifest must not add an account");
    const auto arguments = setupWith(QStringLiteral("/manifest.json"));
    const auto inspected
        = QCoro::waitFor(registry.callSource(setup->sourceId(), QStringLiteral("inspectConnections"), arguments));
    require(inspected.value("origins").toList().isEmpty(), "the add-on does not request any other connections");
    const auto configured
        = QCoro::waitFor(registry.callSource(setup->sourceId(), QStringLiteral("configure"), arguments));
    QString accountId;
    QObject::connect(&registry, &ProviderRegistry::accountAdded, &registry, [&](const QString& id) { accountId = id; });
    setup->complete(configured);
    QElapsedTimer startup;
    startup.start();
    while ((!hub.source(accountId) || !hub.source(accountId)->ready()) && startup.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    require(!accountId.isEmpty() && hub.source(accountId) && hub.source(accountId)->ready(),
        "start the configured add-on account");

    const auto libraries = QCoro::waitFor(hub.fetchLibraries());
    require(libraries.size() == 1, "list the add-on's movie catalogue");
    const auto page = QCoro::waitFor(hub.fetchBrowsePage(
        BrowseDescriptor::library(libraries.front().id, libraries.front().collectionType), 0, 10, {}, std::nullopt));
    require(
        page.items.size() == 1 && hub.accountOf(page.items.front().id) == accountId, "browse one scoped fixture item");
    const auto item = QCoro::waitFor(hub.fetchItemDetails(page.items.front().id));
    require(
        item.id == page.items.front().id && item.title == QLatin1String("Synthetic Test Film"), "read add-on metadata");
    const auto found = QCoro::waitFor(hub.searchItems(QStringLiteral("Synthetic")));
    require(found.size() == 1 && found.front().id == item.id, "search returns the scoped add-on item");
    require(hub.capabilities().testFlag(Provider::Search) && hub.capabilities().testFlag(Provider::Downloads)
            && !hub.capabilities().testAnyFlags(Provider::UserItemState | Provider::PlaybackReporting
                | Provider::GroupPlayback | Provider::RemoteControl | Provider::StreamQuality),
        "the running add-on exposes supported actions and gates server state and controls");
    rejected = false;
    try {
        QCoro::waitFor(hub.setItemFavorite(item.id, true));
    } catch (const std::exception& error) {
        rejected = QByteArray(error.what()) == QByteArrayLiteral("unsupported_capability");
    }
    require(rejected && !QCoro::waitFor(hub.fetchItemDetails(item.id)).favorite,
        "an unavailable favorite operation is rejected without changing state");

    bool cancel = true;
    QObject::connect(&registry, &ProviderRegistry::componentRequested, &registry, [&](QObject *object) {
        auto *picker = qobject_cast<ProviderUiContext *>(object);
        require(picker && picker->role() == QLatin1String("picker"), "resolve opens the provider stream picker");
        if (cancel) {
            picker->close();
            return;
        }
        const auto streams
            = QCoro::waitFor(registry.callSource(accountId, QStringLiteral("streams"), picker->arguments()))
                  .value("items")
                  .toList();
        require(streams.size() == 1 && !streams.front().toMap().value("disabled").toBool(),
            "offer the trusted HTTP stream");
        QVariantMap selection { { "itemId", SourceHub::rawId(item.id) },
            { "stream", streams.front().toMap().value("id") } };
        const auto destination = QCoro::waitFor(registry.callSource(accountId, QStringLiteral("prepare"), selection));
        require(destination.value("approved").toBool(), "playback uses the already-approved add-on origin");
        picker->complete(selection);
    });
    rejected = false;
    try {
        QCoro::waitFor(hub.playback()->resolvePlayback(item, false));
    } catch (const std::exception& error) {
        rejected = QByteArray(error.what()) == QByteArrayLiteral("playback_cancelled");
    }
    require(rejected, "dismissing the picker cancels playback");
    cancel = false;
    const auto playback = QCoro::waitFor(hub.playback()->resolvePlayback(item, false));
    require(playback.itemId == item.id && playback.url == origin + QStringLiteral("/fixture.mp4")
            && playback.playMethod == QLatin1String("DirectPlay"),
        "the host resolves the authorized HTTP stream for the scoped item");
    ProviderCompatibility::verifyDecodedVideo(playback);
    DownloadRequest request;
    request.itemId = item.id;
    const auto original = QCoro::waitFor(hub.negotiateDownload(request, QStringLiteral("stremio-fixture")));
    require(original.url == QUrl(playback.url) && original.container == QLatin1String("mp4") && original.size > 0,
        "negotiate the original file with its actual media length");
    request.transcode = true;
    rejected = false;
    try {
        QCoro::waitFor(hub.negotiateDownload(request, QStringLiteral("stremio-fixture")));
    } catch (const std::exception& error) {
        rejected = QByteArray(error.what()) == QByteArrayLiteral("download_unavailable");
    }
    require(rejected, "reject a converted download the running add-on does not support");
    return 0;
}
