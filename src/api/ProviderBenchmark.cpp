#include "ProviderBenchmark.h"

#include "JellyfinApiFacade.h"
#include "JellyfinJsAdapter.h"
#include "JellyfinProvider.h"
#include "app/SessionController.h"
#include "cache/DatabaseManager.h"
#include "provider/ProviderRegistry.h"

#include <QCoroTimer>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUuid>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <numeric>
#include <vector>

namespace JellyfinNative {

namespace {

    struct BenchResult {
        QString name;
        std::vector<double> nativeTimes;
        std::vector<double> jsTimes;
        size_t nativeItems = 0;
        size_t jsItems = 0;

        double nativeAvg() const
        {
            return nativeTimes.empty()
                ? 0.0
                : std::accumulate(nativeTimes.begin(), nativeTimes.end(), 0.0) / nativeTimes.size();
        }
        double jsAvg() const
        {
            return jsTimes.empty() ? 0.0 : std::accumulate(jsTimes.begin(), jsTimes.end(), 0.0) / jsTimes.size();
        }
        double deltaMs() const
        {
            return jsAvg() - nativeAvg();
        }
        double deltaFrames() const
        {
            return deltaMs() / 16.666667;
        }
    };

} // namespace

QCoro::Task<int> ProviderBenchmark::run(DatabaseManager *database, ProviderRegistry *registry,
    JellyfinProvider *jellyfinProvider, const ProviderBenchmarkOptions& options)
{
    if (!jellyfinProvider || !database || !registry) {
        std::cerr << "provider benchmark: invalid setup\n";
        co_return 1;
    }

    SessionController *sessionCtrl = jellyfinProvider->session();
    if (!sessionCtrl || !sessionCtrl->authenticated()) {
        StartupState startupState = co_await database->loadStartupStateAsync(jellyfinProvider->startupStorageKeys());
        QString deviceId = std::move(startupState.deviceId);
        if (deviceId.isEmpty()) {
            deviceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            database->saveDeviceId(deviceId);
        }
        jellyfinProvider->setDeviceId(deviceId);
        jellyfinProvider->restoreFromStorage(std::move(startupState.values), std::move(startupState.profiles));

        if (sessionCtrl && !sessionCtrl->authenticated() && !sessionCtrl->accountProfiles().isEmpty()) {
            const QVariantMap profile = sessionCtrl->accountProfiles().front().toMap();
            const QString profileId = profile.value(QStringLiteral("id")).toString();
            sessionCtrl->activateProfile(profileId);
            int attempts = 0;
            while (!sessionCtrl->authenticated() && attempts++ < 50) {
                co_await QCoro::sleepFor(std::chrono::milliseconds(20));
            }
        }
    }

    if (!sessionCtrl || !sessionCtrl->authenticated()) {
        std::cerr << "provider benchmark: no authenticated session; sign in to run benchmark\n";
        co_return 1;
    }

    JellyfinApiFacade *nativeApi = jellyfinProvider->api();
    if (!nativeApi) {
        std::cerr << "provider benchmark: native API facade unavailable\n";
        co_return 1;
    }

    const QString serverUrl = sessionCtrl->serverUrl();
    const AuthSession auth = nativeApi->session();
    std::cout << "\n================================================================================\n";
    std::cout << " SPOOL PROVIDER PERFORMANCE BENCHMARK (NATIVE VS JAVASCRIPT)\n";
    std::cout << " Server: " << serverUrl.toStdString() << " | User: " << auth.userName.toStdString() << '\n';
    std::cout << " Iterations: " << options.iterations << " | Library: " << options.libraryName.toStdString() << '\n';
    std::cout << "================================================================================\n\n";

    // Setup JS Adapter
    JellyfinJsAdapter jsAdapter(registry);
    QVariantMap config;
    config.insert(QStringLiteral("server"), serverUrl);
    config.insert(QStringLiteral("userId"), auth.userId);
    config.insert(QStringLiteral("token"), auth.accessToken);
    config.insert(QStringLiteral("deviceId"),
        nativeApi->deviceId().isEmpty() ? QStringLiteral("spool-benchmark") : nativeApi->deviceId());
    config.insert(QStringLiteral("deviceName"), QStringLiteral("Spool Benchmark"));
    config.insert(QStringLiteral("clientVersion"), QStringLiteral("0.7.15"));

    QList<QUrl> origins { QUrl(serverUrl) };
    const QString sourceId = co_await registry->configureSource(
        QStringLiteral("spool.jellyfin"), auth.userId, QStringLiteral("library"), serverUrl, config, origins);
    jsAdapter.configure(sourceId, serverUrl, auth.accessToken);

    std::vector<BenchResult> results;

    // 1. Fetch Libraries
    try {
        BenchResult res;
        res.name = QStringLiteral("fetchLibraries");
        std::cout << "Benchmarking " << res.name.toStdString() << "..." << std::flush;
        // Warmup
        co_await nativeApi->fetchLibraries();
        co_await jsAdapter.fetchLibraries();

        for (int i = 0; i < options.iterations; ++i) {
            QElapsedTimer t;
            t.start();
            auto nativeLibs = co_await nativeApi->fetchLibraries();
            res.nativeTimes.push_back(t.nsecsElapsed() / 1000000.0);
            res.nativeItems = nativeLibs.size();

            t.restart();
            auto jsLibs = co_await jsAdapter.fetchLibraries();
            res.jsTimes.push_back(t.nsecsElapsed() / 1000000.0);
            res.jsItems = jsLibs.size();
        }
        std::cout << " done.\n";
        results.push_back(std::move(res));
    } catch (const std::exception& e) {
        std::cout << " failed: " << e.what() << "\n";
    }

    // Find the target library descriptor
    BrowseDescriptor libDesc;
    try {
        auto libs = co_await nativeApi->fetchLibraries();
        for (const auto& lib : libs) {
            if (lib.name.compare(options.libraryName, Qt::CaseInsensitive) == 0) {
                libDesc.id = lib.id;
                libDesc.name = lib.name;
                libDesc.collectionType = lib.collectionType;
                libDesc.kind = BrowseKind::Library;
                break;
            }
        }
        if (!libDesc.isValid() && !libs.empty()) {
            libDesc.id = libs[0].id;
            libDesc.name = libs[0].name;
            libDesc.collectionType = libs[0].collectionType;
            libDesc.kind = BrowseKind::Library;
        }
    } catch (const std::exception& e) {
        std::cout << "Failed to query libraries for browsing: " << e.what() << "\n";
    }

    // 2. Browse Page 1 (100 items)
    if (libDesc.isValid()) {
        try {
            BenchResult res;
            res.name = QStringLiteral("browsePage: ") + libDesc.name + QStringLiteral(" [0..100]");
            std::cout << "Benchmarking " << res.name.toStdString() << "..." << std::flush;
            co_await nativeApi->fetchBrowsePage(libDesc, 0, 100);
            co_await jsAdapter.fetchBrowsePage(libDesc, 0, 100);

            for (int i = 0; i < options.iterations; ++i) {
                QElapsedTimer t;
                t.start();
                auto page = co_await nativeApi->fetchBrowsePage(libDesc, 0, 100);
                res.nativeTimes.push_back(t.nsecsElapsed() / 1000000.0);
                res.nativeItems = page.items.size();

                t.restart();
                auto jsPage = co_await jsAdapter.fetchBrowsePage(libDesc, 0, 100);
                res.jsTimes.push_back(t.nsecsElapsed() / 1000000.0);
                res.jsItems = jsPage.items.size();
            }
            std::cout << " done.\n";
            results.push_back(std::move(res));
        } catch (const std::exception& e) {
            std::cout << " failed: " << e.what() << "\n";
        }
    }

    // 3. Browse Page 2 (100 items)
    if (libDesc.isValid()) {
        try {
            BenchResult res;
            res.name = QStringLiteral("browsePage: ") + libDesc.name + QStringLiteral(" [100..200]");
            std::cout << "Benchmarking " << res.name.toStdString() << "..." << std::flush;
            co_await nativeApi->fetchBrowsePage(libDesc, 100, 100);
            co_await jsAdapter.fetchBrowsePage(libDesc, 100, 100);

            for (int i = 0; i < options.iterations; ++i) {
                QElapsedTimer t;
                t.start();
                auto page = co_await nativeApi->fetchBrowsePage(libDesc, 100, 100);
                res.nativeTimes.push_back(t.nsecsElapsed() / 1000000.0);
                res.nativeItems = page.items.size();

                t.restart();
                auto jsPage = co_await jsAdapter.fetchBrowsePage(libDesc, 100, 100);
                res.jsTimes.push_back(t.nsecsElapsed() / 1000000.0);
                res.jsItems = jsPage.items.size();
            }
            std::cout << " done.\n";
            results.push_back(std::move(res));
        } catch (const std::exception& e) {
            std::cout << " failed: " << e.what() << "\n";
        }
    }

    // 4. Search
    try {
        BenchResult res;
        res.name = QStringLiteral("searchItems (\"") + options.searchQuery + QStringLiteral("\", 80)");
        std::cout << "Benchmarking " << res.name.toStdString() << "..." << std::flush;
        co_await nativeApi->searchItems(options.searchQuery, 80);
        co_await jsAdapter.searchItems(options.searchQuery, 80);

        for (int i = 0; i < options.iterations; ++i) {
            QElapsedTimer t;
            t.start();
            auto items = co_await nativeApi->searchItems(options.searchQuery, 80);
            res.nativeTimes.push_back(t.nsecsElapsed() / 1000000.0);
            res.nativeItems = items.size();

            t.restart();
            auto jsItems = co_await jsAdapter.searchItems(options.searchQuery, 80);
            res.jsTimes.push_back(t.nsecsElapsed() / 1000000.0);
            res.jsItems = jsItems.size();
        }
        std::cout << " done.\n";
        results.push_back(std::move(res));
    } catch (const std::exception& e) {
        std::cout << " failed: " << e.what() << "\n";
    }

    // 5. Next Up
    try {
        BenchResult res;
        res.name = QStringLiteral("fetchNextUpEpisodes (24)");
        std::cout << "Benchmarking " << res.name.toStdString() << "..." << std::flush;
        co_await nativeApi->fetchNextUpEpisodes(24);
        co_await jsAdapter.fetchNextUpEpisodes(24);

        for (int i = 0; i < options.iterations; ++i) {
            QElapsedTimer t;
            t.start();
            auto items = co_await nativeApi->fetchNextUpEpisodes(24);
            res.nativeTimes.push_back(t.nsecsElapsed() / 1000000.0);
            res.nativeItems = items.size();

            t.restart();
            auto jsItems = co_await jsAdapter.fetchNextUpEpisodes(24);
            res.jsTimes.push_back(t.nsecsElapsed() / 1000000.0);
            res.jsItems = jsItems.size();
        }
        std::cout << " done.\n";
        results.push_back(std::move(res));
    } catch (const std::exception& e) {
        std::cout << " failed: " << e.what() << "\n";
    }

    // 6. Resume Items
    try {
        BenchResult res;
        res.name = QStringLiteral("fetchResumeItems (24)");
        std::cout << "Benchmarking " << res.name.toStdString() << "..." << std::flush;
        co_await nativeApi->fetchResumeItems(24);
        co_await jsAdapter.fetchResumeItems(24);

        for (int i = 0; i < options.iterations; ++i) {
            QElapsedTimer t;
            t.start();
            auto items = co_await nativeApi->fetchResumeItems(24);
            res.nativeTimes.push_back(t.nsecsElapsed() / 1000000.0);
            res.nativeItems = items.size();

            t.restart();
            auto jsItems = co_await jsAdapter.fetchResumeItems(24);
            res.jsTimes.push_back(t.nsecsElapsed() / 1000000.0);
            res.jsItems = jsItems.size();
        }
        std::cout << " done.\n";
        results.push_back(std::move(res));
    } catch (const std::exception& e) {
        std::cout << " failed: " << e.what() << "\n";
    }

    // Print summary table
    std::cout << "\n---------------------------------------------------------------------------------------------------"
                 "---------------\n";
    printf("| %-36s | %-12s | %-12s | %-11s | %-15s | %-10s |\n", "Operation", "Native (ms)", "JS (ms)", "Delta (ms)",
        "Delta (frames)", "Items (N/J)");
    std::cout << "-----------------------------------------------------------------------------------------------------"
                 "-------------\n";

    QJsonArray jsonResults;
    for (const auto& r : results) {
        const double delta = r.deltaMs();
        const double frames = r.deltaFrames();
        char itemsBuf[32];
        snprintf(itemsBuf, sizeof(itemsBuf), "%zu / %zu", r.nativeItems, r.jsItems);
        printf("| %-36s | %9.2f ms | %9.2f ms | %+8.2f ms | %+11.2f fr | %-10s |\n", qPrintable(r.name), r.nativeAvg(),
            r.jsAvg(), delta, frames, itemsBuf);

        QJsonObject obj;
        obj.insert(QStringLiteral("operation"), r.name);
        obj.insert(QStringLiteral("nativeAvgMs"), r.nativeAvg());
        obj.insert(QStringLiteral("jsAvgMs"), r.jsAvg());
        obj.insert(QStringLiteral("deltaMs"), delta);
        obj.insert(QStringLiteral("deltaFrames"), frames);
        obj.insert(QStringLiteral("nativeItems"), static_cast<qint64>(r.nativeItems));
        obj.insert(QStringLiteral("jsItems"), static_cast<qint64>(r.jsItems));
        jsonResults.append(obj);
    }
    std::cout << "-----------------------------------------------------------------------------------------------------"
                 "-------------\n";
    std::cout << " Note: 1 frame = 16.67 ms at 60 Hz display refresh rate.\n";
    std::cout << " Negative delta means JavaScript backend was FASTER than native.\n\n";

    if (!options.outputPath.isEmpty()) {
        QJsonObject root;
        root.insert(QStringLiteral("server"), serverUrl);
        root.insert(QStringLiteral("iterations"), options.iterations);
        root.insert(QStringLiteral("results"), jsonResults);
        QFile outFile(options.outputPath);
        if (outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            outFile.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            std::cout << "Benchmark report saved to: " << options.outputPath.toStdString() << '\n';
        }
    }

    co_return 0;
}

} // namespace JellyfinNative
