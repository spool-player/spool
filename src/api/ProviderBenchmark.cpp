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
        std::vector<double> times;
        size_t items = 0;

        double avg() const
        {
            return times.empty() ? 0.0 : std::accumulate(times.begin(), times.end(), 0.0) / times.size();
        }
        double min() const
        {
            return times.empty() ? 0.0 : *std::min_element(times.begin(), times.end());
        }
        double max() const
        {
            return times.empty() ? 0.0 : *std::max_element(times.begin(), times.end());
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
    std::cout << " SPOOL JELLYFIN PROVIDER PERFORMANCE BENCHMARK\n";
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
    config.insert(QStringLiteral("clientVersion"), QStringLiteral("0.8.1"));

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
        co_await jsAdapter.fetchLibraries();

        for (int i = 0; i < options.iterations; ++i) {
            QElapsedTimer t;
            t.start();
            auto jsLibs = co_await jsAdapter.fetchLibraries();
            res.times.push_back(t.nsecsElapsed() / 1000000.0);
            res.items = jsLibs.size();
        }
        std::cout << " done.\n";
        results.push_back(std::move(res));
    } catch (const std::exception& e) {
        std::cout << " failed: " << e.what() << "\n";
    }

    // Find the target library descriptor
    BrowseDescriptor libDesc;
    try {
        auto libs = co_await jsAdapter.fetchLibraries();
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
            co_await jsAdapter.fetchBrowsePage(libDesc, 0, 100);

            for (int i = 0; i < options.iterations; ++i) {
                QElapsedTimer t;
                t.start();
                auto jsPage = co_await jsAdapter.fetchBrowsePage(libDesc, 0, 100);
                res.times.push_back(t.nsecsElapsed() / 1000000.0);
                res.items = jsPage.items.size();
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
            co_await jsAdapter.fetchBrowsePage(libDesc, 100, 100);

            for (int i = 0; i < options.iterations; ++i) {
                QElapsedTimer t;
                t.start();
                auto jsPage = co_await jsAdapter.fetchBrowsePage(libDesc, 100, 100);
                res.times.push_back(t.nsecsElapsed() / 1000000.0);
                res.items = jsPage.items.size();
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
        co_await jsAdapter.searchItems(options.searchQuery, 80);

        for (int i = 0; i < options.iterations; ++i) {
            QElapsedTimer t;
            t.start();
            auto jsItems = co_await jsAdapter.searchItems(options.searchQuery, 80);
            res.times.push_back(t.nsecsElapsed() / 1000000.0);
            res.items = jsItems.size();
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
        co_await jsAdapter.fetchNextUpEpisodes(24);

        for (int i = 0; i < options.iterations; ++i) {
            QElapsedTimer t;
            t.start();
            auto jsItems = co_await jsAdapter.fetchNextUpEpisodes(24);
            res.times.push_back(t.nsecsElapsed() / 1000000.0);
            res.items = jsItems.size();
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
        co_await jsAdapter.fetchResumeItems(24);

        for (int i = 0; i < options.iterations; ++i) {
            QElapsedTimer t;
            t.start();
            auto jsItems = co_await jsAdapter.fetchResumeItems(24);
            res.times.push_back(t.nsecsElapsed() / 1000000.0);
            res.items = jsItems.size();
        }
        std::cout << " done.\n";
        results.push_back(std::move(res));
    } catch (const std::exception& e) {
        std::cout << " failed: " << e.what() << "\n";
    }

    // Print summary table
    std::cout << "\n---------------------------------------------------------------------------------------\n";
    printf("| %-42s | %-10s | %-10s | %-10s | %-6s |\n", "Operation", "Avg (ms)", "Min (ms)", "Max (ms)", "Items");
    std::cout << "---------------------------------------------------------------------------------------\n";

    QJsonArray jsonResults;
    for (const auto& r : results) {
        printf("| %-42s | %7.2f ms | %7.2f ms | %7.2f ms | %-6zu |\n", qPrintable(r.name), r.avg(), r.min(), r.max(),
            r.items);

        QJsonObject obj;
        obj.insert(QStringLiteral("operation"), r.name);
        obj.insert(QStringLiteral("avgMs"), r.avg());
        obj.insert(QStringLiteral("minMs"), r.min());
        obj.insert(QStringLiteral("maxMs"), r.max());
        obj.insert(QStringLiteral("items"), static_cast<qint64>(r.items));
        jsonResults.append(obj);
    }
    std::cout << "---------------------------------------------------------------------------------------\n\n";

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
