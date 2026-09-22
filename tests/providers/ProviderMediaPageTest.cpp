#include "provider/ProviderMediaPage.h"
#include "TestMain.h"
#include "provider/ScriptRuntime.h"

#include <QCoreApplication>
#include <QJSEngine>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
template <typename T> void rejects(QCoro::Task<T> task, const char *message)
{
    bool rejected = false;
    try {
        QCoro::waitFor(std::move(task));
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, message);
}
}

JELLYFIN_TEST_MAIN("provider-media-page")
{
    QCoreApplication app(argc, argv);
    using namespace JellyfinNative;
    QJSEngine engine;
    const auto read = [&](const QString& expression, int limit = 100) {
        const QJSValue value = engine.evaluate(expression);
        require(!value.isError(), "fixture is valid JavaScript");
        return Detail::readProviderMediaPage(value, limit);
    };
    const auto invalid = [&](const QString& expression, int limit = 100) {
        bool rejected = false;
        try {
            read(expression, limit);
        } catch (const std::exception& error) {
            require(QByteArray(error.what()) == "invalid_media_page", "errors contain no provider data");
            rejected = true;
        }
        if (!rejected)
            std::cerr << expression.left(120).toStdString() << '\n';
        require(rejected, "invalid typed page must reject");
    };
    const auto page = read(QStringLiteral(R"JS(({
        total: 2, exhausted: false, cursor: 'opaque:next', items: [{
            id: 'film', title: 'Example', type: 'Movie', year: 2020,
            externalIds: {IMDb: 'tt123', Tmdb: '456', tvdb: '789'},
            runtimeTicks: '9007199254740993', resumeTicks: '123456789',
            favorite: true, played: false, genres: ['Drama'], people: [{id: 'p', name: 'Person'}]
        }]
    }))JS"));
    const auto& movie = page.items.front();
    require(movie.id == "film" && movie.title == "Example" && movie.sortName == movie.title,
        "native listing fields and fallback sort title are populated");
    require(movie.runtimeTicks == 9007199254740993LL && movie.resumeTicks == 123456789,
        "exact decimal ticks never round through a JS double");
    require(movie.imdbId == "tt123" && movie.tmdbId == "456", "external identifiers are matched without case");
    require(page.cursor == QStringLiteral("opaque:next") && page.total == 2 && !page.exhausted,
        "pagination retains opaque cursor, known total and exhaustion separately");
    require(movie.genres.size() == 1 && movie.people.size() == 1 && movie.favorite,
        "bounded metadata lists and native flags are copied");
    const auto empty = read(QStringLiteral("({items: [], total: null, cursor: null, exhausted: true})"));
    require(empty.items.empty() && !empty.total && !empty.cursor && empty.exhausted,
        "unknown total is not a fabricated zero");
    const auto sparse = read(QStringLiteral("({items: [{id:'x', year:null, season:0, episode:0}], exhausted:true})"));
    require(sparse.items.front().year == 0 && sparse.items.front().seasonNumber == 0,
        "nullable metadata and season zero are valid");
    read(QStringLiteral("({items: [], cursor:'next', exhausted:false})")); // sparse filtered page may advance
    invalid(QStringLiteral("null"));
    invalid(QStringLiteral("({items: {}, exhausted: true})"));
    invalid(QStringLiteral("({items: [], exhausted: 'true'})"));
    invalid(QStringLiteral("({items: [], exhausted: false, cursor: null})"));
    invalid(QStringLiteral("({items: [], exhausted: false, cursor: 7})"));
    invalid(QStringLiteral("({items: [], exhausted: true, total: -1})"));
    invalid(QStringLiteral("({items: [{title:'no id'}], exhausted: true})"));
    invalid(QStringLiteral("({items: [{id:'x', runtimeTicks:9007199254740992}], exhausted:true})"));
    invalid(QStringLiteral("({items: [{id:'x', runtimeTicks:'9223372036854775808'}], exhausted:true})"));
    invalid(QStringLiteral("({items: [{id:'x', runtimeTicks:'1e3'}], exhausted:true})"));
    invalid(QStringLiteral("({items: [{id:'x', year:2020.5}], exhausted:true})"));
    invalid(QStringLiteral("({items: [{id:'x', title:42}], exhausted:true})"));
    invalid(QStringLiteral("({items: [{id:'x', favorite:'false'}], exhausted:true})"));
    invalid(QStringLiteral("({items: [{id:'x'},{id:'y'}], exhausted:true})"), 1);
    invalid(QStringLiteral("({items: [], exhausted:true})"), 0);
    invalid(QStringLiteral("({items: [{id:'x', title:'x'.repeat(65537)}], exhausted:true})"));

    const QString entry = QStringLiteral(TEST_SOURCE_DIR "/tests/providers/fixtures/provider.mjs");
    ScriptRuntime runtime(entry, QVariantMap {});
    QCoro::waitFor(runtime.addSource("a", { { "label", "A" } }, {}));
    QCoro::waitFor(runtime.addSource("b", { { "label", "B" } }, {}));
    auto a = runtime.callMediaPage("a", "mediaPage", { { "count", 72 } });
    auto b = runtime.callMediaPage("b", "mediaPage", { { "count", 72 } });
    const auto resultA = QCoro::waitFor(std::move(a));
    const auto resultB = QCoro::waitFor(std::move(b));
    require(resultA.items.size() == 72 && resultB.items.size() == 72, "worker delivers complete typed pages");
    require(resultA.items.front().id == resultB.items.front().id,
        "sources are isolated; the hub, not the worker, scopes overlapping IDs");
    rejects(runtime.callMediaPage("a", "mediaPage", { { "count", 4 } }, {}, 3), "requested output bound enforced");
    rejects(runtime.callMediaPage("a", "mediaPage", { { "count", 1 } }, {}, 0), "invalid caller bound rejected");
    auto cancelled = runtime.callMediaPage("a", "delayedMediaPage", {}, "closed-action");
    runtime.cancelScope("a", "closed-action");
    rejects(std::move(cancelled), "typed page operations share scope cancellation");
    require(QCoro::waitFor(runtime.call("b", "state")).contains("calls"),
        "small RPC path still works after typed-page validation failure/cancellation");
    require(QCoro::waitFor(runtime.callMediaPage("a", "mediaPage", { { "count", 1 } })).items.size() == 1,
        "invalid result does not kill the source");
    return 0;
}
