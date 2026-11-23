#include "PlayQueueController.h"

#include "../common/AsyncTask.h"
#include "../provider/PlaybackSource.h"

#include <QDebug>
#include <QRandomGenerator>
#include <algorithm>
#include <numeric>
#include <utility>

namespace Spool {

namespace {

    QString displayTitle(const MovieItem& item)
    {
        return item.title.isEmpty() ? item.seriesName : item.title;
    }

    QString familyName(const QString& name)
    {
        const qsizetype separator = name.lastIndexOf(QLatin1Char(' '));
        return separator < 0 ? QString() : name.mid(separator + 1);
    }

    // Siblings and spouses direct together often enough to be worth the
    // collapse: "Joel Coen, Ethan Coen" reads as "Joel and Ethan Coen".
    void collapseSharedFamilyName(QStringList& names)
    {
        if (names.size() < 2)
            return;
        const QString family = familyName(names.constLast());
        if (family.isEmpty())
            return;
        for (const QString& name : names) {
            if (familyName(name) != family)
                return;
        }
        for (qsizetype index = 0; index < names.size() - 1; ++index)
            names[index].chop(family.size() + 1);
    }

    QString joinNames(const QStringList& names)
    {
        if (names.size() < 2)
            return names.value(0);
        const QStringList leading = names.mid(0, names.size() - 1);
        return leading.join(QStringLiteral(", ")) + QStringLiteral(" and ") + names.constLast();
    }

    QString directorText(const MovieItem& item)
    {
        constexpr qsizetype maxNames = 3;
        QStringList names;
        qsizetype directorCount = 0;
        for (const PersonItem& person : item.people) {
            if (person.type != QStringLiteral("Director") || person.name.isEmpty())
                continue;
            if (names.size() < maxNames)
                names.push_back(person.name);
            ++directorCount;
        }
        const qsizetype hiddenCount = directorCount - names.size();
        if (hiddenCount > 0)
            return QStringLiteral("%1 +%2 more").arg(names.join(QStringLiteral(", "))).arg(hiddenCount);
        collapseSharedFamilyName(names);
        return joinNames(names);
    }

    double playbackProgress(const MovieItem& item)
    {
        const qint64 resumeTicks = normalizedResumeTicks(item.resumeTicks, item.runtimeTicks);
        if (resumeTicks <= 0 || item.runtimeTicks <= 0)
            return 0.0;
        return std::clamp(static_cast<double>(resumeTicks) / static_cast<double>(item.runtimeTicks), 0.0, 1.0);
    }

    QVariantMap itemSnapshot(const MovieItem& item)
    {
        return {
            { QStringLiteral("movieId"), item.id },
            { QStringLiteral("playlistItemId"), item.playlistItemId },
            { QStringLiteral("title"), item.title },
            { QStringLiteral("displayTitle"), displayTitle(item) },
            { QStringLiteral("displaySubtitle"), itemDisplaySubtitle(item) },
            { QStringLiteral("itemType"), item.itemType },
            { QStringLiteral("seriesId"), item.seriesId },
            { QStringLiteral("seasonId"), item.seasonId },
            { QStringLiteral("seriesName"), item.seriesName },
            { QStringLiteral("album"), item.album },
            { QStringLiteral("albumId"), item.albumId },
            { QStringLiteral("albumArtist"), item.albumArtist },
            { QStringLiteral("albumPrimaryImageTag"), item.albumPrimaryImageTag },
            { QStringLiteral("posterTag"), item.posterTag },
            { QStringLiteral("year"), item.year },
            { QStringLiteral("director"), directorText(item) },
            { QStringLiteral("seasonNumber"), item.seasonNumber },
            { QStringLiteral("episodeNumber"), item.episodeNumber },
            { QStringLiteral("episodeCode"), itemEpisodeCode(item) },
            { QStringLiteral("genericEpisodeTitle"), isGenericEpisodeTitle(item) },
            { QStringLiteral("playable"), isPlayableItem(item) },
            { QStringLiteral("resumeTicks"), item.resumeTicks },
            { QStringLiteral("played"), item.played },
            { QStringLiteral("runtimeTicks"), item.runtimeTicks },
        };
    }

} // namespace

PlayQueueController::PlayQueueController(PlaybackSource *api, QObject *parent)
    : QAbstractListModel(parent)
    , m_api(api)
    , m_outline(new PlayQueueOutlineModel(this, this))
{
}

int PlayQueueController::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_entries.size());
}

QVariant PlayQueueController::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount())
        return {};

    const MovieItem& item = m_entries[static_cast<size_t>(index.row())];
    switch (role) {
    case ItemIdRole:
        return item.id;
    case PlaylistItemIdRole:
        return item.playlistItemId;
    case TitleRole:
        return item.title;
    case DisplayTitleRole:
        return displayTitle(item);
    case DisplaySubtitleRole:
        return itemDisplaySubtitle(item);
    case ItemTypeRole:
        return item.itemType;
    case SeriesIdRole:
        return item.seriesId;
    case SeasonIdRole:
        return item.seasonId;
    case SeriesNameRole:
        return item.seriesName;
    case YearRole:
        return item.year;
    case SeasonNumberRole:
        return item.seasonNumber;
    case EpisodeNumberRole:
        return item.episodeNumber;
    case EpisodeCodeRole:
        return itemEpisodeCode(item);
    case GenericEpisodeTitleRole:
        return isGenericEpisodeTitle(item);
    case PlayableRole:
        return isPlayableItem(item);
    case ItemRole:
        return QVariant::fromValue(item);
    case ProgressRole:
        return playbackProgress(item);
    case UserQueuedRole:
        return isUserQueued(index.row());
    default:
        return {};
    }
}

QHash<int, QByteArray> PlayQueueController::roleNames() const
{
    return {
        { ItemIdRole, "movieId" },
        { PlaylistItemIdRole, "playlistItemId" },
        { TitleRole, "title" },
        { DisplayTitleRole, "displayTitle" },
        { DisplaySubtitleRole, "displaySubtitle" },
        { ItemTypeRole, "itemType" },
        { SeriesIdRole, "seriesId" },
        { SeasonIdRole, "seasonId" },
        { SeriesNameRole, "seriesName" },
        { YearRole, "year" },
        { SeasonNumberRole, "seasonNumber" },
        { EpisodeNumberRole, "episodeNumber" },
        { EpisodeCodeRole, "episodeCode" },
        { GenericEpisodeTitleRole, "genericEpisodeTitle" },
        { PlayableRole, "playable" },
        { ItemRole, "item" },
        { ProgressRole, "progress" },
        { UserQueuedRole, "userQueued" },
    };
}

std::vector<PlaybackQueueItem> PlayQueueController::nowPlayingQueue() const
{
    std::vector<PlaybackQueueItem> queue;
    queue.reserve(m_order.size());
    for (int naturalIndex : m_order) {
        if (naturalIndex < 0 || naturalIndex >= rowCount())
            continue;
        const MovieItem& item = m_entries[static_cast<size_t>(naturalIndex)];
        queue.push_back({ item.id, item.playlistItemId });
    }
    return queue;
}

bool PlayQueueController::matchesQueue(const std::vector<MovieItem>& items, int currentIndex) const
{
    if (m_shuffled || currentIndex != this->currentIndex() || items.size() != m_entries.size())
        return false;

    for (size_t row = 0; row < items.size(); ++row) {
        const MovieItem& incoming = items[row];
        const MovieItem& held = m_entries[row];
        // playlistItemId is the group's identity for a row; ids alone would
        // call two copies of the same track in one queue identical.
        if (incoming.playlistItemId != held.playlistItemId || incoming.id != held.id)
            return false;
    }
    return true;
}

QVariantMap PlayQueueController::get(int index) const
{
    if (index < 0 || index >= rowCount())
        return {};
    return itemSnapshot(m_entries[static_cast<size_t>(index)]);
}

bool PlayQueueController::hasPlaylistItems() const
{
    return std::any_of(
        m_entries.cbegin(), m_entries.cend(), [](const MovieItem& item) { return !item.playlistItemId.isEmpty(); });
}

MovieItem PlayQueueController::nextUnplayedEpisode(const MovieItem& episode) const
{
    if (episode.seriesId.isEmpty())
        return {};
    const MovieItem *successor = nullptr;
    const auto completedOrder = std::pair(episode.seasonNumber, episode.episodeNumber);
    for (const MovieItem& candidate : m_entries) {
        const auto order = std::pair(candidate.seasonNumber, candidate.episodeNumber);
        if (candidate.itemType != QStringLiteral("Episode") || !isPlayableItem(candidate) || candidate.played
            || candidate.seriesId != episode.seriesId || order <= completedOrder)
            continue;
        if (!successor || order < std::pair(successor->seasonNumber, successor->episodeNumber))
            successor = &candidate;
    }
    return successor ? *successor : MovieItem {};
}

bool PlayQueueController::updateResumeTicks(const QString& itemId, qint64 resumeTicks)
{
    if (itemId.isEmpty())
        return false;

    bool updated = false;
    for (int row = 0; row < rowCount(); ++row) {
        MovieItem& item = m_entries[static_cast<size_t>(row)];
        const qint64 normalizedTicks = normalizedResumeTicks(resumeTicks, item.runtimeTicks);
        if (item.id != itemId || item.resumeTicks == normalizedTicks)
            continue;
        item.resumeTicks = normalizedTicks;
        updated = true;
        // The queue is a visible list now, so a silent write leaves stale
        // progress on screen until something else rebuilds the delegates.
        const QModelIndex changed = index(row);
        emit dataChanged(changed, changed, { ItemRole, ProgressRole });
    }
    return updated;
}

bool PlayQueueController::updatePlayed(const QString& itemId, bool played)
{
    if (itemId.isEmpty())
        return false;
    bool updated = false;
    for (int row = 0; row < rowCount(); ++row) {
        MovieItem& item = m_entries[static_cast<size_t>(row)];
        if (item.id != itemId || (item.played == played && item.resumeTicks == 0))
            continue;
        item.played = played;
        item.resumeTicks = 0;
        updated = true;
        const QModelIndex changed = index(row);
        emit dataChanged(changed, changed, { ItemRole, ProgressRole });
    }
    return updated;
}

bool PlayQueueController::updatePeople(const QString& itemId, const QList<PersonItem>& people)
{
    if (itemId.isEmpty())
        return false;

    bool updated = false;
    for (int row = 0; row < rowCount(); ++row) {
        MovieItem& item = m_entries[static_cast<size_t>(row)];
        if (item.id != itemId || item.people == people)
            continue;
        item.people = people;
        updated = true;
        const QModelIndex changed = index(row);
        emit dataChanged(changed, changed, { ItemRole });
    }
    if (updated)
        emit queueChanged();
    return updated;
}

bool PlayQueueController::next()
{
    if (!canGoNext())
        return false;
    setCurrentOrderIndex((m_orderIndex + 1) % static_cast<int>(m_order.size()));
    return true;
}

bool PlayQueueController::previous()
{
    if (!canGoPrevious())
        return false;
    setCurrentOrderIndex(m_orderIndex > 0 ? m_orderIndex - 1 : static_cast<int>(m_order.size()) - 1);
    return true;
}

bool PlayQueueController::advanceAfterCompletion()
{
    if (m_repeatMode == QStringLiteral("RepeatOne"))
        return currentIndex() >= 0;
    return next();
}

void PlayQueueController::setRepeatMode(const QString& mode)
{
    if (mode == m_repeatMode
        || (mode != QStringLiteral("RepeatNone") && mode != QStringLiteral("RepeatAll")
            && mode != QStringLiteral("RepeatOne")))
        return;
    m_repeatMode = mode;
    // Wrapping changes the availability of Next/Previous without moving the
    // cursor; queueChanged reevaluates those bindings.
    emit queueChanged();
}

bool PlayQueueController::playAt(int index)
{
    if (index < 0 || index >= rowCount())
        return false;
    const auto current = std::find(m_order.begin(), m_order.end(), index);
    if (current == m_order.end())
        return false;
    setCurrentOrderIndex(static_cast<int>(std::distance(m_order.begin(), current)));
    return true;
}

void PlayQueueController::setShuffled(bool shuffled)
{
    if (m_shuffled == shuffled)
        return;

    const int previousCurrent = currentIndex();
    m_shuffled = shuffled;
    if (m_entries.empty()) {
        emit queueChanged();
        return;
    }

    if (m_shuffled)
        rebuildShuffledOrder(previousCurrent);
    else {
        rebuildNaturalOrder();
        m_orderIndex = previousCurrent >= 0 ? previousCurrent : 0;
    }
    emitQueueStateChanged(previousCurrent);
}

bool PlayQueueController::moveItem(int from, int to)
{
    return moveRange(from, 1, to);
}

bool PlayQueueController::removeItem(int index)
{
    if (index < 0 || index >= rowCount())
        return false;

    const int previousCurrent = currentIndex();

    beginRemoveRows({}, index, index);
    m_entries.erase(m_entries.begin() + index);
    m_userQueued.erase(m_userQueued.begin() + index);
    endRemoveRows();

    // Drop the row from the play order and relabel the natural indices above
    // it. Rebuilding instead would throw away a shuffle the user did not ask to
    // end, which is what removing a row used to do.
    const auto removed = std::find(m_order.begin(), m_order.end(), index);
    const int removedOrderPos = removed == m_order.end() ? -1 : static_cast<int>(removed - m_order.begin());
    if (removed != m_order.end())
        m_order.erase(removed);
    for (int& natural : m_order) {
        if (natural > index)
            --natural;
    }

    if (m_order.empty()) {
        m_orderIndex = -1;
    } else {
        // Removing the playing row leaves the cursor in place, which now names
        // whatever followed it; the caller restarts playback on that entry.
        if (removedOrderPos >= 0 && removedOrderPos < m_orderIndex)
            --m_orderIndex;
        m_orderIndex = std::clamp(m_orderIndex, 0, static_cast<int>(m_order.size()) - 1);
    }

    emitQueueStateChanged(previousCurrent);
    return true;
}

void PlayQueueController::clear()
{
    cancelEpisodeSuccessors();
    setRepeatMode(QStringLiteral("RepeatNone"));
    if (m_entries.empty())
        return;
    const int previousCurrent = currentIndex();
    beginResetModel();
    m_entries.clear();
    m_userQueued.clear();
    m_order.clear();
    m_orderIndex = -1;
    m_shuffled = false;
    endResetModel();
    emitQueueStateChanged(previousCurrent);
}

bool PlayQueueController::playNow(const std::vector<MovieItem>& items, int startIndex, bool userQueued)
{
    if (startIndex < 0 || startIndex >= static_cast<int>(items.size())
        || !isQueueable(items[static_cast<size_t>(startIndex)])) {
        return false;
    }

    std::vector<MovieItem> nextEntries;
    nextEntries.reserve(items.size());
    int nextCurrent = -1;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        if (!isQueueable(items[static_cast<size_t>(i)]))
            continue;
        if (i == startIndex)
            nextCurrent = static_cast<int>(nextEntries.size());
        nextEntries.push_back(items[static_cast<size_t>(i)]);
    }
    if (nextCurrent < 0)
        return false;

    cancelEpisodeSuccessors();
    const int previousCurrent = currentIndex();
    beginResetModel();
    m_entries = std::move(nextEntries);
    m_userQueued.assign(m_entries.size(), userQueued);
    rebuildNaturalOrder();
    m_orderIndex = nextCurrent;
    if (m_shuffled)
        rebuildShuffledOrder(nextCurrent);
    endResetModel();
    emitQueueStateChanged(previousCurrent);
    return true;
}

bool PlayQueueController::playNow(const MovieItem& item, bool userQueued)
{
    return playNow(std::vector<MovieItem> { item }, 0, userQueued);
}

bool PlayQueueController::playNext(const MovieItem& item)
{
    if (!isQueueable(item))
        return false;

    if (m_entries.empty())
        return playNow(item, true);

    const int previousCurrent = currentIndex();
    // Land beside the playing row, not at the end. Appending put the entry at
    // the bottom of the list while playing it next, so a visible queue would
    // contradict the order it is meant to describe.
    const int naturalIndex = previousCurrent >= 0 ? previousCurrent + 1 : rowCount();

    beginInsertRows({}, naturalIndex, naturalIndex);
    m_entries.insert(m_entries.begin() + naturalIndex, item);
    m_userQueued.insert(m_userQueued.begin() + naturalIndex, true);
    endInsertRows();

    // Relabel before inserting, so the new row's own index is not bumped too.
    for (int& natural : m_order) {
        if (natural >= naturalIndex)
            ++natural;
    }

    const int insertOrderIndex = m_orderIndex >= 0 ? m_orderIndex + 1 : 0;
    m_order.insert(m_order.begin() + insertOrderIndex, naturalIndex);
    if (m_orderIndex < 0)
        m_orderIndex = insertOrderIndex;
    emitQueueStateChanged(previousCurrent);
    return true;
}

bool PlayQueueController::addToQueue(const MovieItem& item)
{
    if (!isQueueable(item))
        return false;

    if (m_entries.empty())
        return playNow(item, true);

    const int previousCurrent = currentIndex();
    const int naturalIndex = rowCount();
    beginInsertRows({}, naturalIndex, naturalIndex);
    m_entries.push_back(item);
    m_userQueued.push_back(true);
    endInsertRows();
    m_order.push_back(naturalIndex);
    emitQueueStateChanged(previousCurrent);
    return true;
}

bool PlayQueueController::addToQueue(const std::vector<MovieItem>& items, bool next)
{
    std::vector<MovieItem> queueable;
    queueable.reserve(items.size());
    std::copy_if(items.begin(), items.end(), std::back_inserter(queueable),
        [](const MovieItem& item) { return isQueueable(item); });
    if (queueable.empty())
        return false;

    if (m_entries.empty())
        return playNow(queueable, 0, true);

    const int previousCurrent = currentIndex();
    // A whole season queued next lands beside the playing row in the order it
    // was given, rather than reversed by inserting each one at the same anchor.
    const int at = next ? (previousCurrent >= 0 ? previousCurrent + 1 : rowCount()) : rowCount();
    const int added = static_cast<int>(queueable.size());

    beginInsertRows({}, at, at + added - 1);
    m_entries.insert(m_entries.begin() + at, queueable.begin(), queueable.end());
    m_userQueued.insert(m_userQueued.begin() + at, static_cast<size_t>(added), true);
    endInsertRows();

    for (int& natural : m_order) {
        if (natural >= at)
            natural += added;
    }
    const int insertOrderIndex = next ? (m_orderIndex >= 0 ? m_orderIndex + 1 : 0) : static_cast<int>(m_order.size());
    std::vector<int> inserted(static_cast<size_t>(added));
    std::iota(inserted.begin(), inserted.end(), at);
    m_order.insert(m_order.begin() + insertOrderIndex, inserted.begin(), inserted.end());
    if (m_orderIndex < 0)
        m_orderIndex = insertOrderIndex;
    emitQueueStateChanged(previousCurrent);
    return true;
}

bool PlayQueueController::moveRange(int from, int count, int to)
{
    // `to` is where the block lands once it has been lifted out, matching
    // moveItem's own erase-then-insert reading of its arguments.
    const int rows = rowCount();
    if (from < 0 || count <= 0 || from >= rows || count > rows - from || to < 0 || to > rows - count || to == from)
        return false;

    const int previousCurrent = currentIndex();
    int nextCurrent = previousCurrent;
    if (previousCurrent >= from && previousCurrent < from + count)
        nextCurrent += to - from;
    else if (to < from && previousCurrent >= to && previousCurrent < from)
        nextCurrent += count;
    else if (to > from && previousCurrent >= from + count && previousCurrent < to + count)
        nextCurrent -= count;

    if (!beginMoveRows({}, from, from + count - 1, {}, to > from ? to + count : to))
        return false;
    // Rotate the block and the rows it crosses once. Moving each member
    // separately shifted the queue, rebuilt its play order and invalidated
    // the outline once per episode in a folded run.
    const auto moveBlock = [from, count, to](auto& values) {
        if (to < from)
            std::rotate(values.begin() + to, values.begin() + from, values.begin() + from + count);
        else
            std::rotate(values.begin() + from, values.begin() + from + count, values.begin() + to + count);
    };
    moveBlock(m_entries);
    moveBlock(m_userQueued);
    // Manual moves end shuffle so the visible order is also the play order.
    // Publish the final cursor before rowsMoved lets the outline inspect it.
    m_shuffled = false;
    rebuildNaturalOrder();
    m_orderIndex = nextCurrent;
    endMoveRows();
    emitQueueStateChanged(previousCurrent);
    return true;
}

void PlayQueueController::enqueueEpisodeSuccessors(const MovieItem& episode)
{
    cancelEpisodeSuccessors();
    const quint64 generation = m_successorGeneration;
    if (!m_api || episode.itemType != QStringLiteral("Episode") || episode.seriesId.isEmpty() || !m_api->signedIn()) {
        emit successorLookupFinished(false);
        return;
    }
    m_successorItemId = episode.id;
    Async::runScoped(
        this, m_api->fetchSeriesEpisodes(episode.seriesId),
        [this, episode, generation](const std::vector<MovieItem>& episodes) {
            if (generation != m_successorGeneration || currentItem().id != episode.id)
                return;
            m_successorItemId.clear();
            auto current = std::find_if(episodes.begin(), episodes.end(),
                [&episode](const MovieItem& candidate) { return candidate.id == episode.id; });
            if (current == episodes.end()) {
                emit successorLookupFinished(false);
                return;
            }
            std::vector<MovieItem> successors;
            std::copy_if(++current, episodes.end(), std::back_inserter(successors),
                [](const MovieItem& item) { return !item.id.isEmpty() && isPlayableItem(item); });
            const bool ready = playNow(successors, 0);
            emit successorLookupFinished(ready);
        },
        [this, generation](const std::exception_ptr& error) {
            if (generation != m_successorGeneration)
                return;
            m_successorItemId.clear();
            qWarning() << "play queue: episode successor lookup failed" << exceptionMessage(error);
            emit successorLookupFinished(false);
        });
}

void PlayQueueController::cancelEpisodeSuccessors()
{
    ++m_successorGeneration;
    m_successorItemId.clear();
}

bool PlayQueueController::isQueueable(const MovieItem& item)
{
    return !item.id.isEmpty() && isPlayableItem(item);
}

void PlayQueueController::rebuildNaturalOrder()
{
    m_order.resize(m_entries.size());
    std::iota(m_order.begin(), m_order.end(), 0);
}

void PlayQueueController::rebuildShuffledOrder(int currentNaturalIndex)
{
    rebuildNaturalOrder();
    if (currentNaturalIndex < 0 || currentNaturalIndex >= rowCount()) {
        m_orderIndex = m_order.empty() ? -1 : 0;
        return;
    }

    auto current = std::find(m_order.begin(), m_order.end(), currentNaturalIndex);
    if (current != m_order.end())
        m_order.erase(current);
    std::shuffle(m_order.begin(), m_order.end(), *QRandomGenerator::global());
    m_order.insert(m_order.begin(), currentNaturalIndex);
    m_orderIndex = 0;
}

void PlayQueueController::setCurrentOrderIndex(int orderIndex)
{
    if (orderIndex < 0 || orderIndex >= static_cast<int>(m_order.size()) || m_orderIndex == orderIndex) {
        return;
    }
    const int previousCurrent = currentIndex();
    m_orderIndex = orderIndex;
    emitQueueStateChanged(previousCurrent);
}

void PlayQueueController::emitQueueStateChanged(int previousCurrentIndex)
{
    // Editing other rows must not strand EOF's pending advance. A newly
    // queued item wins over automatic series expansion; ownership changes
    // cancel expansion, while harmless edits leave the lookup running.
    emit queueChanged();
    if (previousCurrentIndex != currentIndex())
        emit currentIndexChanged();
    if (!m_successorItemId.isEmpty()) {
        if (currentItem().id != m_successorItemId) {
            cancelEpisodeSuccessors();
            emit successorLookupFinished(false);
        } else if (canGoNext()) {
            cancelEpisodeSuccessors();
            const bool ready = next();
            emit successorLookupFinished(ready);
        }
    }
}

} // namespace Spool
