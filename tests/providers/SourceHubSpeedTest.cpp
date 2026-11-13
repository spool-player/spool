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
QVariantMap transportSample(
    double rate, int requests = 1, int http = 11, qint64 receive = 0, qint64 range = 512 * 1024, quint64 samples = 1)
{
    return { { "requests", requests }, { "http-version", http }, { "receive-profile", receive ? 1 : 0 },
        { "receive-bytes", receive }, { "range-bytes", range }, { "ordered-bytes-per-second", rate },
        { "ttfb-ms", 120.0 }, { "updated-at-ms", QDateTime::currentMSecsSinceEpoch() }, { "source", 2 },
        { "samples", samples } };
}
QVariantMap feedback(quint64 stream, double rate, int requests = 1)
{
    return { { "stream-id", stream }, { "capacity-samples", 1 }, { "selected-requests", requests },
        { "observed-at-ms", QDateTime::currentMSecsSinceEpoch() }, { "primary-origin-match", true },
        { "transport-samples", QVariantList { transportSample(rate, requests) } } };
}
QVariantMap sampleFor(const QVariantMap& profile, int requests, int http, qint64 receive, qint64 range)
{
    for (const QVariant& value : profile.value("transport-samples").toList()) {
        const auto sample = value.toMap();
        if (sample.value("requests").toInt() == requests && sample.value("http-version").toInt() == http
            && sample.value("receive-profile").toInt() == (receive ? 1 : 0)
            && sample.value("receive-bytes").toLongLong() == receive
            && sample.value("range-bytes").toLongLong() == range)
            return sample;
    }
    return {};
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
                            'ttfb-ms':120, 'elapsed-seconds':0.4, 'received-bytes':4194304};
                    })};
            });
        },
        resolve(args, host) {
            host.emit('resolvedPreferences', {
                bitrate: args.preferredMaxBitrate, height: args.preferredMaxHeight,
                local: args.unlimitedLocalNetwork, remux: args.preferRemux
            });
            return {url:configuration.origin + '/movie?token=private-token', variantId:'v'};
        }
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
    QString melbourneEndpoint;
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
        QHash<QString, QVariantMap> resolvedPreferences;
        QObject::connect(&hub, &SourceHub::accountEvent,
            [&](const QString& account, const QString& type, const QVariantMap& payload) {
                if (type == "probeStarted") {
                    ++started;
                    require(++active == 1, "probes never benchmark against another account");
                } else if (type == "probeFinished") {
                    ++finished;
                    --active;
                } else if (type == "resolvedPreferences") {
                    resolvedPreferences.insert(account, payload);
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
        hub.setPlaybackPreferences(90'000'000, false, true, 2160);
        bool custom = true;
        hub.setPlaybackPreferenceResolver([&](const QString& account) {
            return account == melbourne && custom ? QVariantMap { { "preferredMaxBitrate", qint64(20'000'000) },
                { "preferredMaxHeight", 1080 }, { "unlimitedLocalNetwork", true }, { "preferRemux", false } }
                                                  : QVariantMap {};
        });
        resolve(melbourne);
        resolve(nj);
        require(resolvedPreferences.value(melbourne).value("bitrate").toLongLong() == 20'000'000
                && resolvedPreferences.value(melbourne).value("height").toInt() == 1080
                && resolvedPreferences.value(melbourne).value("local").toBool()
                && !resolvedPreferences.value(melbourne).value("remux").toBool()
                && resolvedPreferences.value(nj).value("bitrate").toLongLong() == 90'000'000
                && resolvedPreferences.value(nj).value("height").toInt() == 2160
                && !resolvedPreferences.value(nj).value("local").toBool()
                && resolvedPreferences.value(nj).value("remux").toBool(),
            "actual script resolve calls receive only their account's quality preferences");
        custom = false;
        hub.refreshPlaybackPreferences();
        resolve(melbourne);
        require(resolvedPreferences.value(melbourne).value("bitrate").toLongLong() == 90'000'000
                && resolvedPreferences.value(melbourne).value("height").toInt() == 2160,
            "clearing a server override restores the global quality preferences at resolve");
        hub.setPlaybackPreferenceResolver({});
        const auto initial = hub.playback()->playbackNetworkProfile();
        require(initial.value("format").toInt() == 2 && initial.value("transport-samples").toList().size() == 3,
            "the complete probe curve reaches native playback as format-2 priors");
        require(initial.value("last-selected-requests").toInt() == 1,
            "the last observed probe count is information, not an app startup decision");
        const auto probePrior = sampleFor(initial, 2, 0, 0, 0);
        require(probePrior.value("source").toInt() == 1 && probePrior.value("samples").toInt() == 1
                && probePrior.value("ordered-bytes-per-second").toDouble() == 20'000'000,
            "probe priors carry unknown protocol/range identity and explicit provenance");
        const QUrl melbourneOrigin = hub.playback()->mediaOrigin();
        auto measured = feedback(1, 20'000'000, 2);
        auto privatePoint = transportSample(20'000'000, 2);
        privatePoint["unexpected-private-field"] = "must-not-persist";
        measured["transport-samples"] = QVariantList { privatePoint };
        hub.playback()->updatePlaybackNetworkProfile(melbourneOrigin, measured);
        require(!sampleFor(hub.playback()->playbackNetworkProfile(), 2, 11, 0, 512 * 1024)
                    .contains("unexpected-private-field"),
            "learning retains only the numeric transport profile contract");
        auto partitioned = feedback(1, 21'000'000, 2);
        partitioned["capacity-samples"] = 2;
        partitioned["transport-samples"] = QVariantList { transportSample(21'000'000, 2, 20),
            transportSample(22'000'000, 2, 30), transportSample(23'000'000, 2, 11, 4 * 1024 * 1024),
            transportSample(24'000'000, 2, 11, 8 * 1024 * 1024), transportSample(25'000'000, 2, 11, 0, 1024 * 1024),
            transportSample(26'000'000, 16) };
        hub.playback()->updatePlaybackNetworkProfile(melbourneOrigin, partitioned);
        auto repeated = feedback(1, 30'000'000, 2);
        repeated["capacity-samples"] = 3;
        repeated["transport-samples"] = QVariantList { transportSample(30'000'000, 2, 11, 0, 512 * 1024, 2) };
        hub.playback()->updatePlaybackNetworkProfile(melbourneOrigin, repeated);
        learnedMelbourne = hub.playback()->playbackNetworkProfile();
        require(learnedMelbourne.value("transport-samples").toList().size() == 10
                && learnedMelbourne.value("last-selected-requests").toInt() == 2,
            "equal sample counters on distinct profiles remain independent");
        const auto tcp = sampleFor(learnedMelbourne, 2, 11, 0, 512 * 1024);
        require(tcp.value("ordered-bytes-per-second").toDouble() == 25'000'000 && tcp.value("samples").toInt() == 2
                && tcp.value("source").toInt() == 2 && tcp.value("ttfb-ms").toDouble() == 120
                && tcp.value("updated-at-ms").toLongLong() > 0 && tcp.size() == 10,
            "only a matching effective profile contributes to its rolling estimate and full wire row");
        require(
            sampleFor(learnedMelbourne, 2, 20, 0, 512 * 1024).value("ordered-bytes-per-second").toDouble() == 21'000'000
                && sampleFor(learnedMelbourne, 2, 30, 0, 512 * 1024).value("ordered-bytes-per-second").toDouble()
                    == 22'000'000
                && sampleFor(learnedMelbourne, 2, 11, 4 * 1024 * 1024, 512 * 1024)
                        .value("ordered-bytes-per-second")
                        .toDouble()
                    == 23'000'000
                && sampleFor(learnedMelbourne, 2, 11, 8 * 1024 * 1024, 512 * 1024)
                        .value("ordered-bytes-per-second")
                        .toDouble()
                    == 24'000'000
                && sampleFor(learnedMelbourne, 2, 11, 0, 1024 * 1024).value("ordered-bytes-per-second").toDouble()
                    == 25'000'000
                && sampleFor(learnedMelbourne, 16, 11, 0, 512 * 1024).value("ordered-bytes-per-second").toDouble()
                    == 26'000'000
                && sampleFor(learnedMelbourne, 2, 0, 0, 0) == probePrior,
            "protocol, receive mode/size, range size, request count and probe evidence never contaminate each other");
        for (int invalid = 0; invalid < 12; ++invalid) {
            auto rejected = feedback(2, 90'000'000, 4);
            if (invalid == 0)
                rejected["observed-at-ms"] = QDateTime::currentMSecsSinceEpoch() - 600'000;
            if (invalid == 1)
                rejected["observed-at-ms"] = QDateTime::currentMSecsSinceEpoch() + 60'000;
            if (invalid == 2)
                rejected["capacity-samples"] = 0;
            if (invalid == 3)
                rejected = repeated;
            if (invalid == 4)
                rejected = feedback(2, std::numeric_limits<double>::quiet_NaN(), 4);
            if (invalid == 5)
                rejected["primary-origin-match"] = false;
            if (invalid == 6) {
                rejected = repeated;
                rejected["capacity-samples"] = 4;
            }
            if (invalid >= 7) {
                auto malformed = transportSample(90'000'000, 8);
                if (invalid == 7)
                    malformed["http-version"] = 21;
                if (invalid == 8)
                    malformed["receive-bytes"] = -1;
                if (invalid == 9)
                    malformed["requests"] = 2.5;
                if (invalid == 10)
                    malformed["source"] = 1;
                rejected["transport-samples"] = QVariantList { transportSample(90'000'000, 4), malformed };
            }
            rejected["average-bytes-per-second"] = 100.0;
            if (invalid == 11)
                rejected["transport-samples"] = QVariantList { transportSample(90'000'000, 4, 0, 0, 0) };
            hub.playback()->updatePlaybackNetworkProfile(melbourneOrigin, rejected);
            require(hub.playback()->playbackNetworkProfile() == learnedMelbourne,
                "stale, replayed, nonprimary and malformed observations cannot partially mutate learning");
        }
        resolve(nj);
        const QUrl njOrigin = hub.playback()->mediaOrigin();
        const auto probe = [&](double singleRate) {
            return registry.recordSpeedTest(nj,
                { { "origin", njOrigin.toString() },
                    { "concurrency-samples",
                        QVariantList { QVariantMap { { "requests", 1 }, { "capacity-bytes-per-second", singleRate },
                                           { "ttfb-ms", 5.0 } },
                            QVariantMap {
                                { "requests", 2 }, { "capacity-bytes-per-second", 12'000'000.0 }, { "ttfb-ms", 5.0 } },
                            QVariantMap { { "requests", 4 }, { "capacity-bytes-per-second", 10'000'000.0 },
                                { "ttfb-ms", 5.0 } } } } });
        };
        require(probe(40'000'000), "negative-scaling probe records");
        hub.playback()->updatePlaybackNetworkProfile(njOrigin, feedback(3, 8'000'000, 4));
        require(hub.playback()->playbackNetworkProfile().value("last-selected-requests").toInt() == 4,
            "profile reports the actual count even when a probe demonstrated a faster count");
        auto actualSingle = feedback(4, 6'000'000);
        actualSingle["transport-samples"] = QVariantList { transportSample(6'000'000, 1) };
        hub.playback()->updatePlaybackNetworkProfile(njOrigin, actualSingle);
        const auto actual = sampleFor(hub.playback()->playbackNetworkProfile(), 1, 11, 0, 512 * 1024);
        require(actual.value("source").toInt() == 2 && actual.value("ordered-bytes-per-second").toDouble() == 6'000'000,
            "actual playback retains its known effective profile separately from probe priors");
        require(probe(90'000'000), "later probe refresh records");
        require(sampleFor(hub.playback()->playbackNetworkProfile(), 1, 11, 0, 512 * 1024) == actual
                && sampleFor(hub.playback()->playbackNetworkProfile(), 4, 11, 0, 512 * 1024)
                        .value("ordered-bytes-per-second")
                        .toDouble()
                    == 8'000'000,
            "a later probe cannot replace actual evidence or merge a different profile");
        hub.playback()->updatePlaybackNetworkProfile(njOrigin, feedback(5, 30'000'000, 16));
        require(hub.playback()->playbackNetworkProfile().value("last-selected-requests").toInt() == 16
                && sampleFor(hub.playback()->playbackNetworkProfile(), 16, 11, 0, 512 * 1024)
                        .value("ordered-bytes-per-second")
                        .toDouble()
                    == 30'000'000,
            "sixteen-request playback supplies learning and the last observed count");
        QVariantList full;
        for (int index = 0; index < 64; ++index)
            full.append(transportSample(10'000'000 + index * 1000, 1, 30, 0, 256 * 1024 + index * 64 * 1024));
        auto bounded = feedback(6, 10'000'000);
        bounded["transport-samples"] = full;
        hub.playback()->updatePlaybackNetworkProfile(njOrigin, bounded);
        const auto learnedNj = hub.playback()->playbackNetworkProfile();
        require(learnedNj.value("transport-samples").toList().size() == 64,
            "an origin retains at most 64 effective profiles after merging new evidence");
        full.append(transportSample(11'000'000, 2, 30));
        bounded["stream-id"] = 7;
        bounded["transport-samples"] = full;
        hub.playback()->updatePlaybackNetworkProfile(njOrigin, bounded);
        require(hub.playback()->playbackNetworkProfile() == learnedNj,
            "oversized feedback is rejected atomically instead of truncating its meaning");
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
                && !learning.contains("private-token") && !learning.contains("https://")
                && !learning.contains("must-not-persist"),
            "learning storage contains only endpoint hashes and numeric data, not addresses or credentials");
        auto profiles = root.value("networkProfiles").toObject();
        melbourneEndpoint = root.value("networkEndpoints").toObject().value(melbourne).toString();
        require(profiles.value(melbourneEndpoint).toObject().value("format").toInt() == 2,
            "the learned payload format is stored on each origin, independent of the account envelope");
        const QString njEndpoint = root.value("networkEndpoints").toObject().value(nj).toString();
        require(profiles.value(njEndpoint).toObject().value("transport-samples").toArray().size() == 64,
            "the persistent origin history itself is bounded, not only the playback view");
        for (auto it = profiles.begin(); it != profiles.end(); ++it) {
            auto profile = it.value().toObject();
            auto samples = profile.value("transport-samples").toArray();
            for (qsizetype index = 0; index < samples.size(); ++index) {
                auto sample = samples[index].toObject();
                sample.insert("updated-at-ms", QDateTime::currentMSecsSinceEpoch() - 31LL * 24 * 60 * 60 * 1000);
                samples[index] = sample;
            }
            profile.insert("transport-samples", samples);
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
        require(profile.value("transport-samples").toList().isEmpty()
                && profile.value("measured-bitrate").toLongLong() == 0
                && profile.value("last-selected-requests").toInt() == 1,
            "expired measurements cannot seed capacity or retain an unsupported observed count");
        auto root = QJsonDocument::fromJson(QCoro::waitFor(database.loadSettingAsync("providers/accounts/2")).toUtf8())
                        .object();
        auto profiles = root.value("networkProfiles").toObject();
        profiles.insert(melbourneEndpoint,
            QJsonObject { { "format", 1 }, { "initial-requests", 16 },
                { "concurrency-samples",
                    QJsonArray { QJsonObject { { "requests", 16 }, { "capacity-bytes-per-second", 99'000'000 },
                        { "updated-at-ms", QDateTime::currentMSecsSinceEpoch() } } } } });
        root.insert("networkProfiles", profiles);
        QVariantMap settings { { "providers/accounts/2",
            QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)) } };
        QCoro::waitFor(database.saveSettings(std::move(settings)));
    }
    {
        DatabaseManager database;
        require(database.initialize(databasePath), "database opens old learned payload");
        ProviderRegistry registry(&database);
        registry.setInstallDirectory(installs);
        registry.loadModules();
        SourceHub hub(&registry);
        QCoro::waitFor(registry.restore());
        hub.setPlaybackActive(true);
        waitUntil([&] { return hub.sources().size() == 3; }, "all accounts survive a learned-format reset");
        MovieItem item;
        item.id = hub.scoped(melbourne, "movie");
        QCoro::waitFor(hub.playback()->resolvePlayback(item, false));
        const auto profile = hub.playback()->playbackNetworkProfile();
        require(profile.value("format").toInt() == 2 && profile.value("transport-samples").toList().isEmpty()
                && profile.value("last-selected-requests").toInt() == 1
                && hub.playback()->mediaOrigin() == QUrl("https://melbourne.invalid"),
            "old learned formats reset without dropping accounts or their restored configuration");
    }
    return 0;
}
