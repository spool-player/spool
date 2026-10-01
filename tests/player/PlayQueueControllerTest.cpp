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

MovieItem item(QString id, QString title, QString playlistItemId = {})
{
    MovieItem movie;
    movie.id = std::move(id);
    movie.title = std::move(title);
    movie.itemType = QStringLiteral("Episode");
    movie.playlistItemId = std::move(playlistItemId);
    return movie;
}

QString idAt(const PlayQueueController& queue, int index)
{
    return queue.get(index).value(QStringLiteral("movieId")).toString();
}

} // namespace

SPOOL_TEST_MAIN("play-queue-controller")
{
    QCoreApplication app(argc, argv);

    PlayQueueController queue;
    std::vector<MovieItem> entries = {
        item(QStringLiteral("a"), QStringLiteral("Episode A"), QStringLiteral("pl-a")),
        item(QStringLiteral("b"), QStringLiteral("Episode B"), QStringLiteral("pl-b")),
        item(QStringLiteral("c"), QStringLiteral("Episode C"), QStringLiteral("pl-c")),
    };
    entries[1].seriesName = QStringLiteral("Queue Show");
    entries[1].year = 2024;
    entries[1].seasonNumber = 3;
    entries[1].episodeNumber = 7;
    entries[1].title = QStringLiteral("Episode 7");
    entries[1].runtimeTicks = 1'000'000'000;
    entries[1].resumeTicks = 200'000'000;
    entries[1].album = QStringLiteral("Queue Album");
    entries[1].albumId = QStringLiteral("album-id");
    entries[1].albumArtist = QStringLiteral("Queue Artist");
    entries[1].albumPrimaryImageTag = QStringLiteral("album-tag");

    require(queue.playNow(entries, 1), "playNow should accept a playable item vector");
    require(queue.count() == 3, "playNow should keep all playable items");
    require(queue.currentIndex() == 1, "playNow should start at requested natural index");
    require(queue.currentItem().id == QStringLiteral("b"), "current item should be requested item");
    const QVariantMap episodeSnapshot = queue.get(1);
    require(episodeSnapshot.value(QStringLiteral("seriesName")).toString() == QStringLiteral("Queue Show")
            && episodeSnapshot.value(QStringLiteral("episodeCode")).toString() == QStringLiteral("S03E07")
            && episodeSnapshot.value(QStringLiteral("year")).toInt() == 2024,
        "queue snapshots should expose series context, episode code, and year");
    require(episodeSnapshot.value(QStringLiteral("genericEpisodeTitle")).toBool(),
        "queue snapshots should identify generic episode titles");
    require(episodeSnapshot.value(QStringLiteral("album")).toString() == QStringLiteral("Queue Album")
            && episodeSnapshot.value(QStringLiteral("albumId")).toString() == QStringLiteral("album-id")
            && episodeSnapshot.value(QStringLiteral("albumArtist")).toString() == QStringLiteral("Queue Artist")
            && episodeSnapshot.value(QStringLiteral("albumPrimaryImageTag")).toString() == QStringLiteral("album-tag"),
        "queue snapshots should preserve album metadata and artwork");
    require(queue.data(queue.index(1), PlayQueueController::YearRole).toInt() == 2024,
        "the queue model should expose the production year role");

    PersonItem director;
    director.name = QStringLiteral("Jane Director");
    director.type = QStringLiteral("Director");
    PersonItem actor;
    actor.name = QStringLiteral("Alex Actor");
    actor.type = QStringLiteral("Actor");
    PersonItem coDirector;
    coDirector.name = QStringLiteral("Sam Co-Director");
    coDirector.type = QStringLiteral("Director");
    PersonItem thirdDirector;
    thirdDirector.name = QStringLiteral("Pat Third");
    thirdDirector.type = QStringLiteral("Director");
    PersonItem fourthDirector;
    fourthDirector.name = QStringLiteral("Robin Fourth");
    fourthDirector.type = QStringLiteral("Director");
    require(queue.updatePeople(QStringLiteral("b"), { director, actor, coDirector, thirdDirector, fourthDirector }),
        "movie credits should update matching queue items");
    require(queue.get(1).value(QStringLiteral("director")).toString()
            == QStringLiteral("Jane Director, Sam Co-Director, Pat Third +1 more"),
        "queue snapshots should cap long director lists and exclude cast members");

    PersonItem joel;
    joel.name = QStringLiteral("Joel Coen");
    joel.type = QStringLiteral("Director");
    PersonItem ethan;
    ethan.name = QStringLiteral("Ethan Coen");
    ethan.type = QStringLiteral("Director");
    require(queue.updatePeople(QStringLiteral("b"), { joel, actor, ethan }),
        "movie credits should update matching queue items");
    require(queue.get(1).value(QStringLiteral("director")).toString() == QStringLiteral("Joel and Ethan Coen"),
        "directors sharing a family name should read as one credit in listed order");

    require(queue.updatePeople(QStringLiteral("b"), { director, coDirector }),
        "movie credits should update matching queue items");
    require(queue.get(1).value(QStringLiteral("director")).toString()
            == QStringLiteral("Jane Director and Sam Co-Director"),
        "two unrelated directors should both be named");

    require(queue.updatePeople(QStringLiteral("b"), { director, actor }),
        "movie credits should update matching queue items");
    require(queue.get(1).value(QStringLiteral("director")).toString() == QStringLiteral("Jane Director"),
        "a single director should read as the name alone");
    require(queue.canGoPrevious() && queue.canGoNext(), "middle current item should allow both directions");
    require(episodeSnapshot.value(QStringLiteral("resumeTicks")).toLongLong() == 200'000'000
            && episodeSnapshot.value(QStringLiteral("runtimeTicks")).toLongLong() == 1'000'000'000,
        "queue snapshots should expose playback progress");
    require(
        queue.updateResumeTicks(QStringLiteral("b"), 350'000'000), "queue progress should update the matching item");
    require(queue.currentItem().resumeTicks == 350'000'000
            && queue.get(1).value(QStringLiteral("resumeTicks")).toLongLong() == 350'000'000,
        "queue playback should use the current resume position");

    const std::vector<PlaybackQueueItem> reported = queue.nowPlayingQueue();
    require(reported.size() == 3, "reported queue should include all queued entries");
    require(reported[1].itemId == QStringLiteral("b") && reported[1].playlistItemId == QStringLiteral("pl-b"),
        "reported queue should preserve item and playlist ids");
    require(episodeSnapshot.value(QStringLiteral("displayTitle")).toString() == QStringLiteral("Episode 7"),
        "episode queue rows should use episode titles rather than series names");

    require(queue.playNext(item(QStringLiteral("d"), QStringLiteral("Episode D"), QStringLiteral("pl-d"))),
        "playNext should accept playable item");
    require(queue.addToQueue(item(QStringLiteral("e"), QStringLiteral("Episode E"), QStringLiteral("pl-e"))),
        "addToQueue should accept playable item");
    require(queue.count() == 5, "queue additions should append rows");
    require(idAt(queue, 2) == QStringLiteral("d"),
        "playNext should land beside the playing row so list order matches play order");
    require(idAt(queue, 4) == QStringLiteral("e"), "addToQueue should land at the end of the list");
    require(queue.next(), "next should move to playNext item");
    require(queue.currentItem().id == QStringLiteral("d"), "playNext item should be first upcoming item");
    require(queue.next(), "next should move past playNext item");
    require(queue.currentItem().id == QStringLiteral("c"), "natural successor should follow inserted playNext item");
    require(queue.next(), "next should move to appended queue item");
    require(queue.currentItem().id == QStringLiteral("e"), "addToQueue item should play after existing upcoming items");
    require(queue.previous(), "previous should move to prior queue entry");
    require(queue.currentItem().id == QStringLiteral("c"), "previous should restore the prior queue entry");
    require(queue.playAt(0), "playAt should accept natural row indexes");
    require(queue.currentItem().id == QStringLiteral("a"), "playAt should switch to requested row");
    require(!queue.playAt(99), "playAt should reject out-of-range rows");
    require(queue.playAt(3), "playAt should restore the natural successor");

    require(queue.moveItem(3, 0), "queue rows should be reorderable");
    require(idAt(queue, 0) == QStringLiteral("c") && queue.currentIndex() == 0,
        "moving the current row should preserve the active item at its new index");
    require(queue.moveItem(2, 1), "non-current queue rows should be reorderable");
    require(idAt(queue, 1) == QStringLiteral("b") && queue.currentItem().id == QStringLiteral("c"),
        "reordering another row should preserve the active item");
    require(queue.removeItem(1), "non-current queue rows should be removable");
    require(
        queue.count() == 4 && idAt(queue, 1) == QStringLiteral("a"), "removing a queue row should close the model gap");
    queue.setShuffled(true);
    require(queue.shuffled(), "setShuffled(true) should enable shuffled mode");
    require(queue.currentItem().id == QStringLiteral("c"), "shuffle should keep current item active");
    queue.setShuffled(false);
    require(!queue.shuffled(), "setShuffled(false) should disable shuffled mode");
    require(queue.currentIndex() == 0, "unshuffle should restore the reordered current index");
    require(idAt(queue, 0) == QStringLiteral("c") && idAt(queue, 3) == QStringLiteral("e"),
        "unshuffle should preserve the explicitly reordered rows");

    require(queue.data(queue.index(0), PlayQueueController::ItemRole).value<MovieItem>().id == QStringLiteral("c"),
        "the item role should carry the whole MovieItem so artwork can resolve every tag");

    // A visible queue has to let you drop the row you are on; refusing left the
    // panel with a row it could not delete and no reason the user could see.
    require(queue.removeItem(queue.currentIndex()), "the active queue row should be removable");
    require(queue.count() == 3 && queue.currentItem().id == QStringLiteral("a"),
        "removing the active row should hand the slot to the following entry");
    require(queue.playAt(2), "playAt should select the final row");
    require(queue.removeItem(2), "the final row should be removable while it is playing");
    require(queue.count() == 2 && queue.currentItem().id == QStringLiteral("d"),
        "removing the final row should fall back to its predecessor");

    queue.setShuffled(true);
    require(queue.removeItem(0), "shuffled queues should still accept removals");
    require(queue.shuffled(), "removing a row should not silently end shuffle");

    // A group broadcast that echoes an edit made here must not rebuild the
    // model, or every row in the queue panel is torn down to end up unchanged.
    require(queue.playNow(entries, 1), "a fresh queue should accept the sample entries");
    // playNow keeps whatever shuffle was set; a group forces it off before
    // hydrating, which is the state matchesQueue is written against.
    queue.setShuffled(false);
    std::vector<MovieItem> mirrored;
    for (int row = 0; row < queue.count(); ++row)
        mirrored.push_back(queue.itemAt(row));
    require(queue.matchesQueue(mirrored, queue.currentIndex()),
        "an identical server queue should be recognised as already held");
    require(!queue.matchesQueue(mirrored, queue.currentIndex() + 1),
        "a different playing position should not count as identical");
    std::vector<MovieItem> reordered = mirrored;
    std::reverse(reordered.begin(), reordered.end());
    require(
        !queue.matchesQueue(reordered, queue.currentIndex()), "a reordered server queue should not count as identical");
    std::vector<MovieItem> shorter = mirrored;
    shorter.pop_back();
    require(!queue.matchesQueue(shorter, queue.currentIndex()), "a shorter server queue should not count as identical");
    queue.setShuffled(true);
    require(!queue.matchesQueue(mirrored, queue.currentIndex()),
        "a shuffled queue plays in a different order than its rows, so it never matches");

    // Starting one thing straight after another leaves the playing row where
    // it was, so nothing about the index changes. The overlay titles itself
    // from currentEntry for exactly this: it has to name the new item.
    std::vector<MovieItem> firstRun = { item(QStringLiteral("s1"), QStringLiteral("First")) };
    firstRun[0].seriesName = QStringLiteral("First Show");
    std::vector<MovieItem> secondRun = { item(QStringLiteral("s2"), QStringLiteral("Second")) };
    secondRun[0].seriesName = QStringLiteral("Second Show");
    require(queue.playNow(firstRun, 0), "a single-item queue should be accepted");
    require(queue.currentEntry().value(QStringLiteral("movieId")).toString() == QStringLiteral("s1"),
        "currentEntry should name the playing item");
    int queueNotifications = 0;
    QObject::connect(
        &queue, &PlayQueueController::queueChanged, &queue, [&queueNotifications]() { ++queueNotifications; });
    const int unchangedIndex = queue.currentIndex();
    require(queue.playNow(secondRun, 0), "a replacement queue should be accepted");
    require(queue.currentIndex() == unchangedIndex, "the replacement should keep the playing row where it was");
    require(queueNotifications > 0, "replacing the queue should notify currentEntry even with the index unmoved");
    require(queue.currentEntry().value(QStringLiteral("movieId")).toString() == QStringLiteral("s2")
            && queue.currentEntry().value(QStringLiteral("seriesName")).toString() == QStringLiteral("Second Show"),
        "currentEntry should follow a queue swapped under an unchanged index");

    require(queue.playNow(entries, 1), "a fresh queue should accept the sample entries");
    require(queue.hasPlaylistItems(), "a queue built from playlist rows should say so");
    require(queue.playNow(secondRun, 0), "a replacement queue should be accepted");
    require(!queue.hasPlaylistItems(), "a queue of plain episodes carries no playlist rows");

    queue.clear();
    require(queue.count() == 0 && queue.currentIndex() == -1, "clear should empty queue and reset current index");
    require(queue.currentEntry().isEmpty(), "an empty queue should have no current entry");

    MovieItem blocked = item(QStringLiteral("x"), QStringLiteral("Blocked"));
    blocked.itemType = QStringLiteral("Series");
    require(!queue.playNow(blocked), "unplayable item should not become the queue");

    return EXIT_SUCCESS;
}
