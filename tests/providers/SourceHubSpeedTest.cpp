#include "ProviderFixture.h"
#include "TestMain.h"
#include "app/AppController.h"
#include "app/ArtworkService.h"
#include "app/BrowseSessionController.h"
#include "cache/DatabaseManager.h"
#include "common/TlsTrust.h"
#include "platform/NativeAppWindow.h"
#include "player/PlayerController.h"
#include "provider/PlaybackSource.h"
#include "provider/ProviderRegistry.h"
#include "provider/SourceHub.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
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
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(condition(), message);
}
}

SPOOL_TEST_MAIN("source-hub-speed-test")
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir directory;
    qputenv("SPOOL_CREDENTIAL_STORE_DIR", directory.filePath("credentials").toUtf8());
    DatabaseManager database;
    require(database.initialize(directory.filePath("cache.sqlite")), "database opens");
    auto package = ProviderFixture::package();
    auto manifest = QJsonDocument::fromJson(package.files.value("manifest.json")).object();
    manifest.insert("capabilities", QJsonArray { "speedTest", "streamQuality" });
    package.files["manifest.json"] = QJsonDocument(manifest).toJson();
    package.manifest = *ProviderManifest::parse(package.files.value("manifest.json"));
    package.files["logic/provider.mjs"] = R"JS(
export function createSource(configuration) {
    return {
        describe() { return {}; },
        speedTest(args, host) {
            host.emit('probeStarted', {});
            return host.delay(40).then(function() {
                host.emit('probeFinished', {});
                return {bitrate: 48000000, parallelRequests: configuration.lanes};
            });
        },
        resolve() { return {url: 'https://media.invalid/movie', variantId: 'v'}; }
    };
}
)JS";
    const QString installs = directory.filePath("providers");
    require(ProviderPackage::install(package, installs).has_value(), "fixture installs");
    ProviderRegistry registry(&database);
    registry.setInstallDirectory(installs);
    registry.loadModules();
    SourceHub hub(&registry);
    QCoro::waitFor(registry.restore());
    hub.setPlaybackActive(true);
    const auto add = [&](const QString& key, int lanes) {
        const QString id = registry.finishSetup({},
            { { "module", "fixture.test" }, { "account", key }, { "label", key },
                { "configuration", QVariantMap { { "lanes", lanes } } } });
        registry.useAccount(id);
        return id;
    };
    const QString a = add("first", 1);
    const QString b = add("second", 4);
    waitUntil([&] { return hub.sources().size() == 2; }, "accounts start");
    int starts = 0;
    int finishes = 0;
    int inFlight = 0;
    bool interrupt = true;
    QObject::connect(&hub, &SourceHub::accountEvent, [&](const QString&, const QString& type, const QVariantMap&) {
        if (type == "probeStarted") {
            ++starts;
            ++inFlight;
            require(inFlight == 1, "accounts benchmark serially, not against each other");
            if (interrupt) {
                interrupt = false;
                hub.setPlaybackActive(true);
                --inFlight;
            }
        } else if (type == "probeFinished") {
            ++finishes;
            --inFlight;
        }
    });
    hub.refreshSpeedTests();
    QCoreApplication::processEvents();
    require(starts == 0, "playback defers explicit speed tests");
    hub.setPlaybackActive(false);
    hub.refreshSpeedTests();
    waitUntil([&] { return starts == 1; }, "idle starts a probe");
    // Let the cancelled operation's original completion time pass.
    QElapsedTimer cancelled;
    cancelled.start();
    waitUntil([&] { return cancelled.elapsed() >= 100; }, "cancelled deadline passes");
    require(finishes == 0 && starts == 1, "starting playback aborts the probe and does not start another account");
    hub.setPlaybackActive(false);
    hub.refreshSpeedTests();
    waitUntil([&] { return finishes == 2; }, "deferred probes complete after playback stops");
    waitUntil(
        [&] {
            return hub.source(a)->playback()->playbackParallelRequests() == 1
                && hub.source(b)->playback()->playbackParallelRequests() == 4;
        },
        "accounts retain distinct measured lane budgets");
    MovieItem item;
    item.id = hub.scoped(a, "movie");
    QCoro::waitFor(hub.playback()->resolvePlayback(item, false));
    require(hub.playback()->playbackParallelRequests() == 1, "first account selects its own network profile");
    item.id = hub.scoped(b, "movie");
    QCoro::waitFor(hub.playback()->resolvePlayback(item, false));
    require(hub.playback()->playbackParallelRequests() == 4, "switching playback switches the measured profile");
    registry.setAccountEnabled(a, false);
    waitUntil([&] { return hub.sources().size() == 1; }, "disabled account leaves browsing");
    hub.setPlaybackActive(true);
    registry.restartAccount(b);
    waitUntil([&] { return hub.source(b) && hub.source(b)->playback()->playbackParallelRequests() == 2; },
        "restarting an account discards its stale measurement");
    NativeAppWindow window(QStringLiteral("spool-speed-test"));
    TlsTrustController trust;
    ArtworkService artwork(directory.filePath("artwork"), 1024 * 1024, 1024 * 1024, 1, &trust);
    PlayerController player(&window, hub.playback(), &trust, {});
    AppController controller(&database, &hub, &artwork, &player);
    hub.setPlaybackActive(false);
    controller.browse()->setLoadingMore(true);
    const int beforeBrowse = starts;
    hub.refreshSpeedTests();
    QElapsedTimer browsing;
    browsing.start();
    waitUntil([&] { return browsing.elapsed() >= 100; }, "foreground browse remains in flight");
    require(starts == beforeBrowse, "foreground catalogue loading defers bandwidth traffic");
    controller.browse()->setLoadingMore(false);
    const int beforeIdle = finishes;
    hub.refreshSpeedTests();
    waitUntil([&] { return finishes == beforeIdle + 1; }, "bandwidth traffic resumes after foreground work");
    return 0;
}
