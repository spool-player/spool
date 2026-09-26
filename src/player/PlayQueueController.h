#pragma once

#include "../media/MediaTypes.h"
// Full definition, not a forward declaration: moc needs the pointed-to type
// of a Q_PROPERTY to be complete. The outline only forward-declares this
// controller in turn, so there is no cycle.
#include "PlayQueueOutlineModel.h"

#include <QAbstractListModel>
#include <QVariantMap>

#include <vector>

namespace Spool {
class PlaybackSource;

class PlayQueueController final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY queueChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(bool shuffled READ shuffled WRITE setShuffled NOTIFY queueChanged)
    Q_PROPERTY(bool canGoNext READ canGoNext NOTIFY currentIndexChanged)
    Q_PROPERTY(bool canGoPrevious READ canGoPrevious NOTIFY currentIndexChanged)
    // What is playing, as the same snapshot get() hands out. A queue can be
    // replaced under an unchanged index -- one item swapped for another at the
    // same position, which is what starting a second thing in a row does -- so
    // a binding written as get(currentIndex) is not told anything happened and
    // goes on naming the item that was playing before. This is notified by
    // every queue mutation, index change or not.
    Q_PROPERTY(QVariantMap currentEntry READ currentEntry NOTIFY queueChanged)
    // Whether the queue is a playlist rather than a run of episodes, which is
    // what decides whether it can be stepped through. Asked of the model so it
    // is answered against the queue as it is now.
    Q_PROPERTY(bool hasPlaylistItems READ hasPlaylistItems NOTIFY queueChanged)
    // The same queue with its automatically filled runs folded up, which is
    // what the panel lists. Owned here so there is one outline per queue.
    Q_PROPERTY(Spool::PlayQueueOutlineModel *outline READ outline CONSTANT)

public:
    enum Roles {
        ItemIdRole = Qt::UserRole + 1,
        PlaylistItemIdRole,
        TitleRole,
        DisplayTitleRole,
        DisplaySubtitleRole,
        ItemTypeRole,
        SeriesIdRole,
        SeasonIdRole,
        SeriesNameRole,
        YearRole,
        SeasonNumberRole,
        EpisodeNumberRole,
        EpisodeCodeRole,
        GenericEpisodeTitleRole,
        PlayableRole,
        // The whole MovieItem, as MovieGridModel already does. Art.url() takes
        // the gadget branch for this and reads every tag, where a QVariantMap
        // only carries the four keys ArtworkService bothers to unpack — so
        // episode thumbs and album covers resolve without new roles per tag.
        ItemRole,
        ProgressRole,
        // Whether this row is here because the user asked for it, rather than
        // because starting an episode filled the queue with its whole series.
        // The outline folds runs of the latter and never the former.
        UserQueuedRole,
    };

    explicit PlayQueueController(PlaybackSource *api = nullptr, QObject *parent = nullptr);

    PlayQueueOutlineModel *outline() const
    {
        return m_outline;
    }

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const
    {
        return rowCount();
    }
    int currentIndex() const
    {
        return m_orderIndex < 0 || m_orderIndex >= static_cast<int>(m_order.size())
            ? -1
            : m_order[static_cast<size_t>(m_orderIndex)];
    }
    bool shuffled() const
    {
        return m_shuffled;
    }
    bool canGoNext() const
    {
        return m_orderIndex >= 0 && m_orderIndex + 1 < static_cast<int>(m_order.size());
    }
    bool canGoPrevious() const
    {
        return m_orderIndex > 0;
    }
    MovieItem itemAt(int index) const
    {
        return index < 0 || index >= rowCount() ? MovieItem {} : m_entries[static_cast<size_t>(index)];
    }
    bool isUserQueued(int index) const
    {
        return index >= 0 && index < rowCount() && m_userQueued[static_cast<size_t>(index)];
    }
    QVariantMap currentEntry() const
    {
        return get(currentIndex());
    }
    bool hasPlaylistItems() const;
    MovieItem currentItem() const
    {
        const int index = currentIndex();
        return index < 0 || index >= rowCount() ? MovieItem {} : m_entries[static_cast<size_t>(index)];
    }
    std::vector<PlaybackQueueItem> nowPlayingQueue() const;
    // Whether an incoming server queue is already what we hold. Rebuilding on a
    // broadcast that echoes our own edit would reset the model and tear down
    // every row the queue panel is showing.
    bool matchesQueue(const std::vector<MovieItem>& items, int currentIndex) const;

    Q_INVOKABLE QVariantMap get(int index) const;
    Q_INVOKABLE bool next();
    Q_INVOKABLE bool previous();
    Q_INVOKABLE bool playAt(int index);
    Q_INVOKABLE void setShuffled(bool shuffled);
    Q_INVOKABLE bool moveItem(int from, int to);
    // Move a whole folded run in one gesture, so dragging a collapsed group
    // takes its hidden members with it.
    Q_INVOKABLE bool moveRange(int from, int count, int to);
    Q_INVOKABLE bool removeItem(int index);
    Q_INVOKABLE void clear();

    bool playNow(const std::vector<MovieItem>& items, int startIndex, bool userQueued = false);
    bool playNow(const MovieItem& item, bool userQueued = false);
    bool playNext(const MovieItem& item);
    bool addToQueue(const MovieItem& item);
    // Queue a whole season or series in one go, either next or at the end.
    bool addToQueue(const std::vector<MovieItem>& items, bool next);
    void enqueueEpisodeSuccessors(const MovieItem& episode);
    bool updateResumeTicks(const QString& itemId, qint64 resumeTicks);
    bool updatePeople(const QString& itemId, const QList<PersonItem>& people);

signals:
    void queueChanged();
    void currentIndexChanged();

    void successorPlaybackReady();

private:
    static bool isQueueable(const MovieItem& item);
    void rebuildNaturalOrder();
    void rebuildShuffledOrder(int currentNaturalIndex);
    void setCurrentOrderIndex(int orderIndex);
    void emitQueueStateChanged(int previousCurrentIndex);

    PlaybackSource *m_api = nullptr;
    PlayQueueOutlineModel *m_outline = nullptr;
    std::vector<MovieItem> m_entries;
    // Parallel to m_entries. Provenance belongs to this queue, not to the
    // item, so it does not go on the shared MovieItem gadget.
    std::vector<bool> m_userQueued;
    std::vector<int> m_order;
    int m_orderIndex = -1;
    bool m_shuffled = false;
};

} // namespace Spool
