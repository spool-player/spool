#include "ProviderCompatibilityVideo.h"
#include "TestMain.h"
#include "TestRequire.h"
#include "cache/DatabaseManager.h"
#include "provider/PlaybackSource.h"
#include "provider/ProviderRegistry.h"
#include "provider/SourceHub.h"

#include <QCoreApplication>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTemporaryDir>
#include <QThread>

using namespace Spool;
using SpoolTests::require;

SPOOL_TEST_MAIN("open-movies-provider")
{
    QCoreApplication app(argc, argv);
    const int port = qEnvironmentVariableIntValue("SPOOL_TEST_OPEN_MOVIES_PROXY");
    if (!port)
        return 77;
    require(port > 0 && port <= 65535, "use the isolated Open Movies test proxy");
    const QString ca = qEnvironmentVariable("SPOOL_TEST_OPEN_MOVIES_CA");
    const auto certificates = QSslCertificate::fromPath(ca);
    require(certificates.size() == 1, "load one fixture-only trust anchor");
    auto tls = QSslConfiguration::defaultConfiguration();
    tls.addCaCertificates(certificates);
    QSslConfiguration::setDefaultConfiguration(tls);

    QTemporaryDir directory;
    require(directory.isValid(), "create a private Open Movies profile");
    qputenv("SPOOL_CREDENTIAL_STORE_DIR", directory.filePath(QStringLiteral("credentials")).toUtf8());
    DatabaseManager database;
    require(database.initialize(directory.filePath(QStringLiteral("cache.sqlite"))), "open the private database");
    ProviderRegistry registry(&database);
    registry.setInstallDirectory(directory.filePath(QStringLiteral("providers")));
    ScriptRuntime::NetworkHooks hooks;
    hooks.network = [port](QNetworkAccessManager *manager) {
        manager->setProxy(QNetworkProxy(QNetworkProxy::HttpProxy, QStringLiteral("127.0.0.1"), quint16(port)));
    };
    registry.setRuntimeEnvironment({ { "id", "spool-open-movies-test" }, { "app", "Spool" }, { "platform", "linux" },
                                       { "version", "0.0.0" }, { "locale", "en" } },
        std::move(hooks));
    registry.loadModules();
    SourceHub hub(&registry);
    QCoro::waitFor(registry.restore());
    QString accountId;
    QObject::connect(&registry, &ProviderRegistry::accountAdded, &registry, [&](const QString& id) { accountId = id; });
    require(
        !registry.beginSetup(QStringLiteral("example.open-movies")), "the public archive needs no account credentials");
    QElapsedTimer timeout;
    timeout.start();
    while (accountId.isEmpty() && timeout.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    require(!accountId.isEmpty() && hub.source(accountId) && hub.source(accountId)->ready(),
        "activate the credential-free archive");
    const auto libraries = QCoro::waitFor(hub.fetchLibraries());
    require(libraries.size() == 1, "list the package's film catalogue");
    const auto page = QCoro::waitFor(hub.fetchBrowsePage(
        BrowseDescriptor::library(libraries.front().id, libraries.front().collectionType), 0, 10, {}, std::nullopt));
    require(page.items.size() >= 2, "browse the fixed catalogue");
    const auto matches = QCoro::waitFor(hub.searchItems(QStringLiteral("Elephants Dream")));
    require(
        matches.size() == 1 && hub.accountOf(matches.front().id) == accountId, "find an account-scoped archive film");
    const QString itemId = matches.front().id;
    bool rejected = false;
    try {
        QCoro::waitFor(hub.fetchItemDetails(itemId));
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "a temporary upstream listing failure is surfaced");
    QDir root(qEnvironmentVariable("SPOOL_TEST_OPEN_MOVIES_ROOT"));
    require(root.rename(QStringLiteral("held"), QStringLiteral("ED")), "publish the fixture's recovered directory");
    const auto details = QCoro::waitFor(hub.fetchItemDetails(itemId));
    require(details.id == itemId && details.mediaSources.size() == 1,
        "retry resolves a playable file and hides the non-media row");
    require(hub.capabilities().testFlag(Provider::Downloads) && !hub.capabilities().testFlag(Provider::UserItemState),
        "the public archive offers downloads and exposes no user state");
    const auto playback = QCoro::waitFor(hub.playback()->resolvePlayback(details, false));
    require(playback.itemId == itemId && QUrl(playback.url).host() == QStringLiteral("download.blender.org"),
        "the app resolves the canonical archive origin and account-scoped item");
    ProviderCompatibility::verifyDecodedVideo(playback, {},
        { { "http-proxy", QByteArrayLiteral("http://127.0.0.1:") + QByteArray::number(port) },
            { "tls-ca-file", ca.toUtf8() }, { "tls-verify", "yes" } });
    DownloadRequest original;
    original.itemId = itemId;
    original.variantId = details.mediaSources.front().id;
    const auto download = QCoro::waitFor(hub.negotiateDownload(original, QStringLiteral("open-movies-fixture")));
    require(download.url == QUrl(playback.url) && download.size > 0,
        "negotiate the same finite original file and its size");
    return 0;
}
