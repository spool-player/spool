#include "app/SearchController.h"
#include "TestMain.h"
#include "provider/SearchSource.h"

#include <QCoreApplication>
#include <QCoroFuture>
#include <QElapsedTimer>
#include <QPromise>
#include <QThread>

#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>

using namespace JellyfinNative;

namespace {
void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class PendingSearch final : public SearchSource {
public:
    bool signedIn() const override
    {
        return true;
    }
    QCoro::Task<std::vector<MovieItem>> searchItems(QString query, int) override
    {
        auto pending = std::make_shared<QPromise<std::vector<MovieItem>>>();
        pending->start();
        requests[query] = pending;
        auto future = pending->future();
        auto awaitable = qCoro(future);
        try {
            auto result = co_await awaitable.takeResult();
            ++completions;
            co_return result;
        } catch (...) {
            ++completions;
            throw;
        }
    }
    QCoro::Task<std::vector<MovieItem>> fetchSearchSuggestions(int) override
    {
        co_return std::vector<MovieItem> {};
    }
    void complete(const QString& query, bool error = false)
    {
        auto pending = requests.at(query);
        if (error) {
            pending->setException(std::make_exception_ptr(std::runtime_error("fixture search failure")));
        } else {
            MovieItem item;
            item.id = query;
            item.title = query;
            item.itemType = QStringLiteral("Movie");
            pending->addResult(std::vector<MovieItem> { item });
        }
        pending->finish();
    }
    void waitForCompletion(int count)
    {
        QElapsedTimer elapsed;
        elapsed.start();
        while (completions < count && elapsed.elapsed() < 2000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        require(completions >= count, "controlled search response must settle");
    }
    int completions = 0;
    std::map<QString, std::shared_ptr<QPromise<std::vector<MovieItem>>>> requests;
};
}

JELLYFIN_TEST_MAIN("search-controller")
{
    QCoreApplication app(argc, argv);
    PendingSearch source;
    SearchController controller(&source, nullptr);
    int errors = 0;
    QObject::connect(&controller, &SearchController::errorOccurred, &app, [&](const QString&) { ++errors; });

    controller.search("alpha");
    controller.setQuery("beta");
    source.complete("alpha");
    source.waitForCompletion(1);
    require(controller.query() == "beta" && controller.resultCount() == 0,
        "a response for the previous query must not publish during the new query's debounce interval");

    controller.submit();
    source.complete("beta");
    source.waitForCompletion(2);
    require(controller.resultCount() == 1 && controller.movieResults()->get(0).title == "beta",
        "the current query publishes its results");

    controller.search("gamma");
    controller.setQuery("delta");
    source.complete("gamma", true);
    source.waitForCompletion(3);
    require(errors == 0 && controller.movieResults()->get(0).title == "beta",
        "a stale failure must neither surface an error nor erase retained results");

    controller.submit();
    controller.clear();
    source.complete("delta");
    source.waitForCompletion(4);
    require(controller.resultCount() == 0 && controller.query().isEmpty() && !controller.busy(),
        "clearing search suppresses late completion");
    return 0;
}
