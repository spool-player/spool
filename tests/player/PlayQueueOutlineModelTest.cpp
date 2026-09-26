#include "player/PlayQueueOutlineModel.h"
#include "player/PlayQueueController.h"

#include "TestMain.h"

#include <QCoreApplication>

#include <cstdlib>
#include <iostream>
#include <vector>

using namespace Spool;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

MovieItem episode(const QString& seriesId, int season, int number)
{
    MovieItem item;
    item.id = QStringLiteral("%1-s%2e%3").arg(seriesId).arg(season).arg(number);
    item.itemType = QStringLiteral("Episode");
    item.seriesId = seriesId;
    item.seriesName = seriesId == QStringLiteral("sev") ? QStringLiteral("Severance") : QStringLiteral("Other Show");
    item.seasonNumber = season;
    item.episodeNumber = number;
    item.title = QStringLiteral("Episode %1").arg(number);
    return item;
}

MovieItem movie(const QString& id)
{
    MovieItem item;
    item.id = id;
    item.itemType = QStringLiteral("Movie");
    item.title = id;
    return item;
}

// One season of a series, as the queue is actually filled: playing an episode
// loads every episode of the show.
std::vector<MovieItem> season(const QString& seriesId, int seasonNumber, int episodes)
{
    std::vector<MovieItem> items;
    for (int number = 1; number <= episodes; ++number)
        items.push_back(episode(seriesId, seasonNumber, number));
    return items;
}

QString kindAt(const PlayQueueOutlineModel& outline, int row)
{
    return outline.data(outline.index(row, 0), PlayQueueOutlineModel::OutlineKindRole).toString();
}

QString labelAt(const PlayQueueOutlineModel& outline, int row)
{
    return outline.data(outline.index(row, 0), PlayQueueOutlineModel::GroupLabelRole).toString();
}

QString sourceIdAt(const PlayQueueOutlineModel& outline, int row)
{
    return outline.data(outline.index(row, 0), PlayQueueController::ItemIdRole).toString();
}

int countAt(const PlayQueueOutlineModel& outline, int row)
{
    return outline.data(outline.index(row, 0), PlayQueueOutlineModel::GroupCountRole).toInt();
}

} // namespace

SPOOL_TEST_MAIN("play-queue-outline-model")
{
    QCoreApplication app(argc, argv);

    // A queue filled by starting the middle of a season, which is the case
    // the whole outline exists for.
    {
        PlayQueueController queue;
        PlayQueueOutlineModel *outline = queue.outline();
        require(outline != nullptr, "the queue should own an outline");

        require(queue.playNow(season(QStringLiteral("sev"), 3, 22), 1), "queue should accept a season");
        require(queue.count() == 22, "queue should hold the season");

        // S03E01, S03E02 (playing), S03E03, and one group for the other 19.
        require(outline->count() == 4, "outline should fold all but the neighbours");
        require(kindAt(*outline, 0) == QStringLiteral("item"), "the previous episode stays visible");
        require(sourceIdAt(*outline, 0) == QStringLiteral("sev-s3e1"), "row 0 should be the previous episode");
        require(sourceIdAt(*outline, 1) == QStringLiteral("sev-s3e2"), "row 1 should be the playing episode");
        require(sourceIdAt(*outline, 2) == QStringLiteral("sev-s3e3"), "row 2 should be the next episode");
        require(kindAt(*outline, 3) == QStringLiteral("group"), "the tail should fold");
        require(countAt(*outline, 3) == 19, "the tail group should stand for 19 episodes");
        require(labelAt(*outline, 3) == QStringLiteral("19 more episodes"), "tail label should count forward");
    }

    // Playing further in gives a group on each side.
    {
        PlayQueueController queue;
        PlayQueueOutlineModel *outline = queue.outline();
        require(queue.playNow(season(QStringLiteral("sev"), 3, 22), 10), "queue should accept a season");

        require(outline->count() == 5, "a run split in the middle folds on both sides");
        require(kindAt(*outline, 0) == QStringLiteral("group"), "the head should fold");
        require(countAt(*outline, 0) == 9, "nine episodes precede the visible neighbour");
        require(labelAt(*outline, 0) == QStringLiteral("9 earlier episodes"), "head label should count back");
        require(sourceIdAt(*outline, 1) == QStringLiteral("sev-s3e10"), "the previous episode stays visible");
        require(sourceIdAt(*outline, 2) == QStringLiteral("sev-s3e11"), "the playing episode stays visible");
        require(sourceIdAt(*outline, 3) == QStringLiteral("sev-s3e12"), "the next episode stays visible");
        require(kindAt(*outline, 4) == QStringLiteral("group"), "the tail should fold");
        require(countAt(*outline, 4) == 10, "ten episodes follow the visible neighbour");
    }

    // A short run is not worth folding: the group row would replace about as
    // many rows as it hides.
    {
        PlayQueueController queue;
        require(queue.playNow(season(QStringLiteral("sev"), 1, 3), 0), "queue should accept a short season");
        require(queue.outline()->count() == 3, "a run under the minimum should stay open");
    }

    // Opening a group shows its members without losing the band that opened
    // it, and closing it puts them back.
    {
        PlayQueueController queue;
        PlayQueueOutlineModel *outline = queue.outline();
        require(queue.playNow(season(QStringLiteral("sev"), 3, 22), 1), "queue should accept a season");
        require(outline->isGroup(3), "row 3 should draw a group");

        require(outline->toggleGroup(3), "the group should open");
        require(outline->count() == 22, "opening should reveal every hidden row");
        require(kindAt(*outline, 3) == QStringLiteral("groupOpen"), "the head keeps drawing the band when open");
        require(outline->data(outline->index(4, 0), PlayQueueOutlineModel::InGroupRole).toBool(),
            "members should be marked as belonging to the group");

        require(outline->toggleGroup(3), "the group should close again");
        require(outline->count() == 4, "closing should fold it back");
    }

    // An opened group stays open as the cursor advances, even though the
    // split that produced it has moved along by one.
    {
        PlayQueueController queue;
        PlayQueueOutlineModel *outline = queue.outline();
        require(queue.playNow(season(QStringLiteral("sev"), 3, 22), 1), "queue should accept a season");
        require(outline->toggleGroup(3), "the tail group should open");
        require(outline->count() == 22, "the tail should be open");

        require(queue.next(), "playback should advance");
        require(outline->count() == 22, "advancing must not close a group the user opened");
    }

    // Rows the user queued are never folded, even mid-run of the same series.
    {
        PlayQueueController queue;
        PlayQueueOutlineModel *outline = queue.outline();
        require(queue.playNow(season(QStringLiteral("sev"), 3, 22), 1), "queue should accept a season");
        require(queue.playNext(episode(QStringLiteral("sev"), 3, 99)), "an episode should queue next");

        // It plays next, so it sits directly after the playing row and is
        // never folded, however long the run around it is.
        require(sourceIdAt(*outline, 1) == QStringLiteral("sev-s3e2"), "the playing row should not move");
        require(sourceIdAt(*outline, 2) == QStringLiteral("sev-s3e99"), "the hand-queued episode plays next");

        // The rest of the series is now cut off from the playing row by that
        // insertion, but it is still the show being watched.
        require(kindAt(*outline, 3) == QStringLiteral("group"), "the remainder still folds");
        // Twenty, not nineteen: the episode that had been visible as "next"
        // is now behind the hand-queued one, so it folds in with the rest.
        require(labelAt(*outline, 3) == QStringLiteral("20 more episodes"),
            "a severed run of the same show still reads as more of it");
    }

    // A run the playing row is nowhere near names itself rather than counting
    // from a cursor that is not in it.
    {
        PlayQueueController queue;
        PlayQueueOutlineModel *outline = queue.outline();
        require(queue.playNow(season(QStringLiteral("sev"), 3, 6), 1), "queue should accept a season");
        require(queue.addToQueue(season(QStringLiteral("other"), 2, 8), false), "a second series should queue");

        int namedRow = -1;
        for (int row = 0; row < outline->count(); ++row) {
            if (labelAt(*outline, row).startsWith(QStringLiteral("Other Show")))
                namedRow = row;
        }
        // Queued by hand, so it is not folded at all -- which is the rule.
        require(namedRow < 0, "a hand-queued series is shown, not folded");
        require(outline->count() >= 8, "its episodes should all be listed");
    }

    // Two automatically filled series in a row fold separately, and the one
    // without the cursor describes itself by name.
    {
        PlayQueueController queue;
        PlayQueueOutlineModel *outline = queue.outline();
        std::vector<MovieItem> mixed = season(QStringLiteral("sev"), 3, 6);
        const std::vector<MovieItem> second = season(QStringLiteral("other"), 2, 8);
        mixed.insert(mixed.end(), second.begin(), second.end());
        require(queue.playNow(mixed, 1), "queue should accept both series");

        int namedRow = -1;
        for (int row = 0; row < outline->count(); ++row) {
            if (labelAt(*outline, row) == QStringLiteral("Other Show · Season 2"))
                namedRow = row;
        }
        require(namedRow >= 0, "the untouched run should name its series and season");
        require(countAt(*outline, namedRow) == 8, "and stand for all of its episodes");
    }

    // Movies and other one-off rows are never part of a run.
    {
        PlayQueueController queue;
        PlayQueueOutlineModel *outline = queue.outline();
        std::vector<MovieItem> mixed = season(QStringLiteral("sev"), 3, 6);
        mixed.insert(mixed.begin() + 3, movie(QStringLiteral("blade-runner")));
        require(queue.playNow(mixed, 0), "queue should accept the mixture");

        bool movieVisible = false;
        for (int row = 0; row < outline->count(); ++row)
            movieVisible = movieVisible || sourceIdAt(*outline, row) == QStringLiteral("blade-runner");
        require(movieVisible, "a film between episodes is never folded away");
    }

    // Mapping both ways, including for a row that is folded out of sight.
    {
        PlayQueueController queue;
        PlayQueueOutlineModel *outline = queue.outline();
        require(queue.playNow(season(QStringLiteral("sev"), 3, 22), 1), "queue should accept a season");

        require(outline->sourceRowAt(0) == 0, "row 0 maps to source row 0");
        require(outline->sourceRowAt(2) == 2, "row 2 maps to source row 2");
        require(outline->rowForSourceRow(1) == 1, "the playing row maps back");
        // Source row 10 is folded into the tail group, which is drawn at row 3.
        require(outline->rowForSourceRow(10) == 3, "a folded row is represented by its group");
        require(outline->currentRow() == 1, "the outline should know where the cursor is");

        const QVariantMap span = outline->groupSpanAt(3);
        require(span.value(QStringLiteral("first")).toInt() == 3, "the tail group starts after the next episode");
        require(span.value(QStringLiteral("count")).toInt() == 19, "and covers the rest of the run");
    }

    // Reordering through the outline moves the underlying rows and the
    // outline follows, rather than going stale.
    {
        PlayQueueController queue;
        PlayQueueOutlineModel *outline = queue.outline();
        std::vector<MovieItem> mixed = season(QStringLiteral("sev"), 3, 6);
        mixed.push_back(movie(QStringLiteral("arrival")));
        require(queue.playNow(mixed, 0), "queue should accept the mixture");

        const int rows = outline->count();
        require(queue.moveItem(6, 0), "the film should move to the front");
        require(sourceIdAt(*outline, 0) == QStringLiteral("arrival"), "the outline should show the move");
        require(outline->count() == rows, "moving a row does not change how many are visible");
    }

    // A whole folded block moves as one.
    {
        PlayQueueController queue;
        std::vector<MovieItem> mixed = season(QStringLiteral("sev"), 3, 6);
        mixed.push_back(movie(QStringLiteral("arrival")));
        require(queue.playNow(mixed, 0), "queue should accept the mixture");

        require(queue.moveRange(2, 3, 4), "a block of three should move");
        require(queue.get(4).value(QStringLiteral("movieId")).toString() == QStringLiteral("sev-s3e3"),
            "the block should arrive intact and in order");
        require(queue.get(5).value(QStringLiteral("movieId")).toString() == QStringLiteral("sev-s3e4"),
            "the block should keep its order");
        require(queue.get(6).value(QStringLiteral("movieId")).toString() == QStringLiteral("sev-s3e5"),
            "the block should keep its order");
        require(!queue.moveRange(0, 3, 0), "leaving a block where it is is not a move");
    }

    // Provenance survives the edits that shuffle rows around.
    {
        PlayQueueController queue;
        require(queue.playNow(season(QStringLiteral("sev"), 3, 6), 0), "queue should accept a season");
        require(!queue.isUserQueued(0), "an automatically filled row is not user queued");
        require(queue.playNext(movie(QStringLiteral("arrival"))), "a film should queue next");
        require(queue.isUserQueued(1), "the hand-queued film should be marked");

        require(queue.moveItem(1, 5), "the film should move");
        require(queue.isUserQueued(5), "provenance should travel with the row");
        require(!queue.isUserQueued(1), "and not be left behind");

        require(queue.removeItem(0), "the first row should go");
        require(queue.isUserQueued(4), "provenance should follow the shift");
    }

    // Queueing a season next keeps its order rather than reversing it.
    {
        PlayQueueController queue;
        require(queue.playNow(season(QStringLiteral("sev"), 3, 4), 0), "queue should accept a season");
        require(queue.addToQueue(season(QStringLiteral("other"), 1, 3), true), "a season should queue next");
        require(queue.get(1).value(QStringLiteral("movieId")).toString() == QStringLiteral("other-s1e1"),
            "the first episode of the added season lands next");
        require(queue.get(3).value(QStringLiteral("movieId")).toString() == QStringLiteral("other-s1e3"),
            "and the rest follow in order");
        require(queue.isUserQueued(2), "the whole added run is user queued");
    }

    // An empty queue accepts a bulk add by starting playback on it.
    {
        PlayQueueController queue;
        require(queue.addToQueue(season(QStringLiteral("sev"), 1, 3), false), "a bulk add should start an empty queue");
        require(queue.count() == 3, "every episode should arrive");
        require(queue.currentIndex() == 0, "and the first should be current");
    }

    return 0;
}
