#include "app/BrowseSessionController.h"

#include "TestMain.h"

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <random>
#include <set>

using Spool::BrowseIndexEntry;
using Spool::BrowseSessionController;
using Spool::LibraryItem;
using Spool::MovieItem;
using Spool::PagedMovieItems;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    qCritical() << message;
    std::exit(EXIT_FAILURE);
}

constexpr int kRows = 2500;
constexpr int kBlock = 100;

// A name-sorted library skewed like a real one (lots of S, no Q or X), with
// plain ASCII titles so the expected order is just a case-insensitive sort.
std::vector<QString> libraryTitles(bool descending)
{
    std::mt19937 random(7);
    const QString common = QStringLiteral("aabbccddeeffgghhiijkllmmmnoopprrsssssstttuvwwyz0");
    std::vector<QString> titles;
    for (int i = 0; i < kRows; ++i) {
        QString title(1, common.at(static_cast<qsizetype>(random() % common.size())).toUpper());
        for (int length = 3 + static_cast<int>(random() % 8); length > 0; --length)
            title.append(QChar(u'a' + static_cast<char16_t>(random() % 26)));
        titles.push_back(title + QString::number(i));
    }
    std::sort(titles.begin(), titles.end(),
        [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    if (descending)
        std::reverse(titles.begin(), titles.end());
    return titles;
}

// Where a file browser lands for typed text: the first title starting with
// it, or failing that the first one past it in listing order.
int expectedRow(const std::vector<QString>& titles, const QString& prefix, bool descending)
{
    for (int row = 0; row < static_cast<int>(titles.size()); ++row) {
        const QString key = titles[static_cast<size_t>(row)].toLower();
        if (descending ? (key < prefix || key.startsWith(prefix)) : key >= prefix)
            return row;
    }
    return static_cast<int>(titles.size()) - 1;
}

MovieItem titleItem(const QString& title, int row)
{
    MovieItem item;
    item.id = QStringLiteral("id%1").arg(row);
    item.title = title;
    item.itemType = QStringLiteral("Movie");
    return item;
}

PagedMovieItems serverPage(const std::vector<QString>& titles, int start)
{
    PagedMovieItems page;
    page.startIndex = start;
    page.limit = kBlock;
    page.totalRecordCount = static_cast<int>(titles.size());
    page.seekable = true;
    const int end = std::min(page.totalRecordCount, start + kBlock);
    for (int row = start; row < end; ++row)
        page.items.push_back(titleItem(titles[static_cast<size_t>(row)], row));
    page.exhausted = end >= page.totalRecordCount;
    if (!page.exhausted)
        page.nextCursor = QString::number(end);
    return page;
}

// Stands in for AppController: block requests are answered later, the way
// a network fetch would be, never from inside the signal.
struct FakeServer {
    BrowseSessionController& session;
    std::vector<QString> titles;
    std::deque<int> requests;
    int served = 0;

    FakeServer(BrowseSessionController& browse, std::vector<QString> rows)
        : session(browse)
        , titles(std::move(rows))
    {
        QObject::connect(&session, &BrowseSessionController::blockRequested, &session,
            [this](int start) { requests.push_back(start); });
    }
    // A view that jumped waits a moment for its next move before asking.
    void waitForRequests()
    {
        QElapsedTimer waited;
        waited.start();
        while (requests.empty() && waited.elapsed() < 400) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
    }
    void deliverAll()
    {
        waitForRequests();
        while (!requests.empty()) {
            const int start = requests.front();
            requests.pop_front();
            ++served;
            session.applyBlock(start, serverPage(titles, start), session.cacheKey());
        }
    }
};

void openLibrary(BrowseSessionController& session, const std::vector<QString>& titles, bool descending)
{
    LibraryItem library;
    library.id = descending ? QStringLiteral("lib-desc") : QStringLiteral("lib");
    library.name = QStringLiteral("Movies");
    library.collectionType = QStringLiteral("movies");
    session.enterLibrary(library,
        { { QStringLiteral("sortBy"), QStringLiteral("SortName") },
            { QStringLiteral("sortOrder"), descending ? QStringLiteral("Descending") : QStringLiteral("Ascending") } });
    session.setPage(serverPage(titles, 0), QStringLiteral("cache:") + library.id, false);
}

// Types `prefix` and runs the seek to completion; the row it settled on.
int seek(FakeServer& server, const QString& prefix, int *fetches)
{
    int settledRow = -1;
    const auto connection
        = QObject::connect(&server.session, &BrowseSessionController::seekMoved, [&settledRow](int row, bool settled) {
              if (settled)
                  settledRow = row;
          });
    const int before = server.served;
    require(server.session.seekPrefix(prefix), "name-sorted sparse listing seeks");
    for (int guard = 0; guard < 50 && server.session.seeking(); ++guard)
        server.deliverAll();
    QObject::disconnect(connection);
    require(!server.session.seeking(), "seek settles");
    *fetches = server.served - before;
    return settledRow;
}

std::vector<BrowseIndexEntry> serverIndex(const std::vector<QString>& ascending)
{
    std::vector<BrowseIndexEntry> entries;
    for (const QString& title : ascending) {
        const QString label = title.at(0).isLetter() ? title.left(1).toUpper() : QStringLiteral("#");
        if (entries.empty() || entries.back().label != label)
            entries.push_back({ label, 0 });
        ++entries.back().count;
    }
    return entries;
}

} // namespace

SPOOL_TEST_MAIN("browse-sparse-listing")
{
    QCoreApplication app(argc, argv);
    QTemporaryDir settingsDir;
    require(settingsDir.isValid(), "temporary settings directory");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings().clear();
    const QStringList prefixes { QStringLiteral("m"), QStringLiteral("ma"), QStringLiteral("st"), QStringLiteral("z"),
        QStringLiteral("a"), QStringLiteral("q"), QStringLiteral("wzz") };

    // A seekable first page with more rows behind it holds only that page.
    {
        BrowseSessionController session(nullptr);
        int indexRequests = 0;
        QObject::connect(&session, &BrowseSessionController::indexRequested, [&indexRequests] { ++indexRequests; });
        const auto titles = libraryTitles(false);
        FakeServer server(session, titles);
        openLibrary(session, titles, false);
        require(session.sparse(), "seekable listing is sparse");
        require(session.items()->count() == kRows, "every row is present");
        require(session.items()->loadedCount() == kBlock, "only the first block is held");
        require(!session.hasMore(), "nothing is paged sequentially");
        require(indexRequests == 1, "a name-sorted listing asks for its index");
        // Without the server's counts the far end becomes an anchor.
        session.setExactIndex({}, session.cacheKey());
        require(server.requests.size() == 1 && server.requests.back() == (kRows - 1) / kBlock * kBlock,
            "the last block is fetched as an anchor");
        server.deliverAll();

        // Jumping deep asks for the rows on screen first.
        session.prefetchVisibleRange(1210, 1245);
        server.waitForRequests();
        require(!server.requests.empty() && server.requests.front() == 1200, "viewport block requested first");
        server.deliverAll();
        require(session.items()->movieAt(1222).title == titles[1222], "block lands on its own rows");
        require(!session.items()->isLoaded(700), "rows far from the viewport stay unloaded");

        session.items()->updateFavorite(QStringLiteral("id1222"), true);
        require(session.items()->movieAt(1222).favorite, "sparse rows take user state updates");

        // Scrolling through the library keeps memory to the budget.
        session.setLoadedRowBudget(400);
        for (int first = 300; first < kRows - 40; first += 230) {
            session.prefetchVisibleRange(first, first + 35);
            server.deliverAll();
            require(session.items()->isLoaded(first) && session.items()->isLoaded(first + 35),
                "the viewport is always loaded after its blocks arrive");
            require(session.items()->loadedCount() <= 400 + 2 * kBlock, "loaded rows stay near the budget");
        }
        require(session.items()->isLoaded(0), "the opening block is kept for the warm cache");

        // Typing still finds the first matching title by estimate.
        for (const QString& prefix : prefixes) {
            int fetches = 0;
            const int row = seek(server, prefix, &fetches);
            require(row == expectedRow(titles, prefix, false), "estimated seek lands on the first match");
            require(fetches <= 6, "estimated seek loads only a few blocks");
        }

        // Once every row has been seen the estimates are the truth.
        session.setLoadedRowBudget(kRows);
        for (int first = 0; first < kRows; first += kBlock * 3) {
            session.prefetchVisibleRange(first, std::min(kRows - 1, first + kBlock * 3 - 1));
            server.deliverAll();
        }
        for (const QVariant& value : session.alphabet()) {
            const QVariantMap letter = value.toMap();
            const QString label = letter.value(QStringLiteral("label")).toString();
            const int expected
                = expectedRow(titles, label == QLatin1String("#") ? QStringLiteral("0") : label.toLower(), false);
            require(letter.value(QStringLiteral("exact")).toBool(), "letters seen on both sides are exact");
            require(letter.value(QStringLiteral("row")).toInt() == expected, "learnt letter starts match the listing");
        }
    }

    // A jump the view has left gives up its fetches, so the next jump is not
    // queued behind rows nobody is looking at any more.
    {
        BrowseSessionController session(nullptr);
        const auto titles = libraryTitles(false);
        FakeServer server(session, titles);
        openLibrary(session, titles, false);
        std::set<int> abandoned;
        QObject::connect(
            &session, &BrowseSessionController::blockAbandoned, [&abandoned](int start) { abandoned.insert(start); });
        session.prefetchVisibleRange(1210, 1245);
        server.waitForRequests();
        const std::deque<int> left = std::exchange(server.requests, {});
        require(left.size() == 3, "a jump fills every fetch slot");
        session.prefetchVisibleRange(2210, 2245);
        require(abandoned == std::set<int>(left.begin(), left.end()), "every fetch for the old jump is given up");
        server.waitForRequests();
        require(!server.requests.empty() && server.requests.front() == 2200, "the new viewport is fetched at once");
        // The given-up fetches report back as failures, as cancelled ones do.
        for (const int start : left)
            session.blockFailed(start);
        server.deliverAll();
        require(session.items()->isLoaded(2222), "the new viewport loads");
        require(!session.items()->isLoaded(1222), "the abandoned rows were never applied");
    }

    // The server's counts place every letter, ascending or descending.
    for (const bool descending : { false, true }) {
        BrowseSessionController session(nullptr);
        const auto titles = libraryTitles(descending);
        FakeServer server(session, titles);
        openLibrary(session, titles, descending);
        session.setExactIndex(serverIndex(libraryTitles(false)), session.cacheKey());
        require(server.requests.empty(), "an exact index needs no anchor block");
        require(session.alphabetExact(), "server counts make the index exact");
        for (const QVariant& value : session.alphabet()) {
            const QVariantMap letter = value.toMap();
            const int row = letter.value(QStringLiteral("row")).toInt();
            const QString label = letter.value(QStringLiteral("label")).toString();
            require(titles[static_cast<size_t>(row)].left(1).toUpper() == label
                    || (label == QLatin1String("#") && !titles[static_cast<size_t>(row)].at(0).isLetter()),
                "an exact letter starts on its first title");
            require(row == 0 || titles[static_cast<size_t>(row - 1)].left(1).toUpper() != label,
                "an exact letter starts no later than its first title");
        }
        for (const QString& prefix : prefixes) {
            int fetches = 0;
            const int row = seek(server, prefix, &fetches);
            require(row == expectedRow(titles, prefix, descending), "seek lands on the first match");
            require(fetches <= (prefix.size() == 1 ? 1 : 5), "an exact index seeks a letter in one fetch");
        }
        require(session.labelAt(1777) == titles[1777].left(1).toUpper() || !titles[1777].at(0).isLetter(),
            "an unloaded row is labelled by the exact index");
    }

    // A listing without positions keeps paging from the top.
    {
        BrowseSessionController session(nullptr);
        const auto titles = libraryTitles(false);
        openLibrary(session, titles, false);
        PagedMovieItems page = serverPage(titles, 0);
        page.seekable = false;
        session.setPage(page, session.cacheKey(), false);
        require(!session.sparse(), "an unseekable listing stays dense");
        require(session.hasMore(), "an unseekable listing pages sequentially");
        require(!session.seekPrefix(QStringLiteral("m")), "a partial dense listing leaves typing to the page");
    }
    return 0;
}
