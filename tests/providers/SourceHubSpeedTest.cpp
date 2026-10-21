#include "ProviderFixture.h"
#include "TestMain.h"
#include "cache/DatabaseManager.h"
#include "provider/PlaybackSource.h"
#include "provider/ProviderRegistry.h"
#include "provider/SourceHub.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>

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
QVariantMap feedback(quint64 stream, double rate, int requests = 1)
{
    return { { "stream-id", stream }, { "capacity-samples", 1 }, { "selected-requests", requests },
        { "observed-at-ms", QDateTime::currentMSecsSinceEpoch() }, { "media-bitrate", 80'000'000 },
        { "concurrency-samples",
            QVariantList { QVariantMap { { "requests", requests }, { "capacity-bytes-per-second", rate },
                { "latency-ms", 120.0 }, { "samples", 1 } } } } };
}
}

SPOOL_TEST_MAIN("source-hub-speed-test")
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    qputenv("SPOOL_CREDENTIAL_STORE_DIR", directory.filePath("credentials").toUtf8());
    auto package = ProviderFixture::package();
    auto manifest = QJsonDocument::fromJson(package.files.value("manifest.json")).object();
    manifest.insert("capabilities", QJsonArray { "speedTest", "streamQuality" });
    manifest.insert("origins", QJsonArray { "https://melbourne.invalid", "https://nj.invalid" });
    package.files["manifest.json"] = QJsonDocument(manifest).toJson();
    package.manifest = *ProviderManifest::parse(package.files.value("manifest.json"));
    package.files["logic/provider.mjs"] = R"JS(
export function createSource(configuration) {
    return {
        describe() { return {capabilities: {speedTest:true, streamQuality:true}}; },
        speedTest(args, host) {
            host.emit('probeStarted', {});
            return host.delay(40).then(function() {
                host.emit('probeFinished', {});
                return {bitrate:126000000, parallelRequests:1, origin:configuration.origin,
                    'concurrency-samples':[1,2,4].map(function(requests, index) {
                        return {requests:requests, 'capacity-bytes-per-second':[10000000,20000000,21000000][index],
                            'latency-ms':120, 'elapsed-seconds':0.4, 'received-bytes':4194304};
                    })};
            });
        },
        resolve() { return {url:configuration.origin + '/movie?token=private-token', variantId:'v'}; }
    };
}
)JS";
    const QString installs = directory.filePath("providers");
    const QString databasePath = directory.filePath("cache.sqlite");
    require(ProviderPackage::install(package, installs).has_value(), "fixture installs");
    QString melbourne;
    QString nj;
    QString local;
    QVariantMap learnedMelbourne;
    {
        DatabaseManager database;
        require(database.initialize(databasePath), "database opens");
        ProviderRegistry registry(&database);
        registry.setInstallDirectory(installs);
        registry.loadModules();
        SourceHub hub(&registry);
        QCoro::waitFor(registry.restore());
        hub.setPlaybackActive(true);
        const auto add = [&](const QString& key, const QString& origin) {
            const QString id = registry.finishSetup({},
                { { "module", "fixture.test" }, { "account", key }, { "label", key },
                    { "configuration", QVariantMap { { "origin", origin } } } });
            registry.useAccount(id);
            return id;
        };
        melbourne = add("Melbourne", "https://melbourne.invalid");
        nj = add("NJ", "https://nj.invalid");
        local = add("Local", "file:");
        waitUntil([&] { return hub.sources().size() == 3; }, "accounts start");
        int started = 0;
        int finished = 0;
        int active = 0;
        QObject::connect(&hub, &SourceHub::accountEvent, [&](const QString&, const QString& type, const QVariantMap&) {
            if (type == "probeStarted") {
                ++started;
                require(++active == 1, "probes never benchmark against another account");
            } else if (type == "probeFinished") {
                ++finished;
                --active;
            }
        });
        hub.refreshSpeedTests();
        QCoreApplication::processEvents();
        require(started == 0, "explicit refresh cannot compete with active playback");
        hub.setPlaybackActive(false);
        hub.refreshSpeedTests();
        waitUntil([&] { return finished == 3; }, "idle explicit probes complete serially");
        require(active == 0, "all probes settle");
        hub.setPlaybackActive(true);
        const auto resolve = [&](const QString& account) {
            MovieItem item;
            item.id = hub.scoped(account, "movie");
            return QCoro::waitFor(hub.playback()->resolvePlayback(item, false));
        };
        resolve(melbourne);
        require(hub.playback()->playbackNetworkProfile().value("concurrency-samples").toList().size() == 3,
            "full benchmark curve reaches native playback without SDK changes");
        require(hub.playback()->playbackNetworkProfile().value("initial-requests").toInt() == 1,
            "no demand starts conservatively instead of choosing the fastest probe");
        const QUrl melbourneOrigin = hub.playback()->mediaOrigin();
        auto measured = feedback(1, 20'000'000, 2);
        hub.playback()->updatePlaybackNetworkProfile(melbourneOrigin, measured);
        learnedMelbourne = hub.playback()->playbackNetworkProfile();
        require(learnedMelbourne.value("initial-requests").toInt() == 2,
            "startup chooses the minimum measured configuration with playback headroom");
        for (int invalid = 0; invalid < 5; ++invalid) {
            auto rejected = feedback(2, 90'000'000, 4);
            if (invalid == 0)
                rejected["observed-at-ms"] = QDateTime::currentMSecsSinceEpoch() - 600'000;
            if (invalid == 1)
                rejected["observed-at-ms"] = QDateTime::currentMSecsSinceEpoch() + 60'000;
            if (invalid == 2)
                rejected["capacity-samples"] = 0;
            if (invalid == 3)
                rejected = feedback(1, 90'000'000, 4);
            if (invalid == 4) {
                rejected = feedback(2, std::numeric_limits<double>::quiet_NaN(), 4);
            }
            rejected["average-bytes-per-second"] = 100.0;
            hub.playback()->updatePlaybackNetworkProfile(melbourneOrigin, rejected);
            require(hub.playback()->playbackNetworkProfile() == learnedMelbourne,
                "stale, replayed, cache-throttled and invalid observations cannot poison learning");
        }
        resolve(nj);
        const QUrl njOrigin = hub.playback()->mediaOrigin();
        hub.playback()->updatePlaybackNetworkProfile(njOrigin, feedback(3, 40'000'000));
        const auto learnedNj = hub.playback()->playbackNetworkProfile();
        require(learnedNj.value("initial-requests").toInt() == 1
                && learnedNj.value("capacity-bytes-per-second").toDouble() > 20'000'000,
            "servers sharing a provider retain independent capacity and startup lanes");
        hub.playback()->updatePlaybackNetworkProfile(melbourneOrigin, feedback(4, 99'000'000));
        require(hub.playback()->playbackNetworkProfile() == learnedNj,
            "delayed feedback from another media origin is not attributed to the current server");
        resolve(local);
        require(hub.playback()->playbackNetworkProfile().value("measured-bitrate").toLongLong() == 0,
            "local media never inherits a remote network capacity");
        resolve(melbourne);
        require(hub.playback()->playbackNetworkProfile() == learnedMelbourne,
            "switching back restores the owning endpoint profile");
    }
    {
        DatabaseManager database;
        require(database.initialize(databasePath), "database reopens");
        ProviderRegistry registry(&database);
        registry.setInstallDirectory(installs);
        registry.loadModules();
        SourceHub hub(&registry);
        QCoro::waitFor(registry.restore());
        hub.setPlaybackActive(true);
        waitUntil([&] { return hub.sources().size() == 3; }, "accounts restore after process lifetime");
        MovieItem item;
        item.id = hub.scoped(melbourne, "movie");
        QCoro::waitFor(hub.playback()->resolvePlayback(item, false));
        require(hub.playback()->playbackNetworkProfile() == learnedMelbourne,
            "actual playback learning survives registry and database restart");
        auto root = QJsonDocument::fromJson(QCoro::waitFor(database.loadSettingAsync("providers/accounts/2")).toUtf8())
                        .object();
        const QByteArray learning = QJsonDocument(QJsonObject { { "profiles", root.value("networkProfiles") },
                                                      { "endpoints", root.value("networkEndpoints") } })
                                        .toJson();
        require(!learning.contains("melbourne") && !learning.contains("nj.invalid")
                && !learning.contains("private-token") && !learning.contains("https://"),
            "learning storage contains only endpoint hashes and numeric data, not addresses or credentials");
        auto profiles = root.value("networkProfiles").toObject();
        for (auto it = profiles.begin(); it != profiles.end(); ++it) {
            auto profile = it.value().toObject();
            for (const QString& field : { QStringLiteral("probe-samples"), QStringLiteral("concurrency-samples") }) {
                auto samples = profile.value(field).toArray();
                for (qsizetype index = 0; index < samples.size(); ++index) {
                    auto sample = samples[index].toObject();
                    sample.insert("updated-at-ms", QDateTime::currentMSecsSinceEpoch() - 31LL * 24 * 60 * 60 * 1000);
                    samples[index] = sample;
                }
                profile.insert(field, samples);
            }
            it.value() = profile;
        }
        root.insert("networkProfiles", profiles);
        QVariantMap settings { { "providers/accounts/2",
            QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)) } };
        QCoro::waitFor(database.saveSettings(std::move(settings)));
    }
    {
        DatabaseManager database;
        require(database.initialize(databasePath), "database opens aged history");
        ProviderRegistry registry(&database);
        registry.setInstallDirectory(installs);
        registry.loadModules();
        SourceHub hub(&registry);
        QCoro::waitFor(registry.restore());
        hub.setPlaybackActive(true);
        waitUntil([&] { return hub.source(melbourne) != nullptr; }, "aged account restores");
        MovieItem item;
        item.id = hub.scoped(melbourne, "movie");
        QCoro::waitFor(hub.playback()->resolvePlayback(item, false));
        const auto profile = hub.playback()->playbackNetworkProfile();
        require(profile.value("concurrency-samples").toList().isEmpty()
                && profile.value("capacity-bytes-per-second").toDouble() == 0
                && profile.value("initial-requests").toInt() == 1,
            "expired measurements cannot seed capacity or retain an unsupported lane selection");
    }
    return 0;
}
