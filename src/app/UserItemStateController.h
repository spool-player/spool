#pragma once

#include "../common/RequestGeneration.h"
#include "../provider/UserItemStateSink.h"

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QString>

#include <exception>

namespace Spool {

class ContentModelController;
class BrowseSessionController;
class HomeModelController;
class SearchController;
struct MovieItem;

class UserItemStateController final : public QObject {
    Q_OBJECT

public:
    UserItemStateController(UserItemStateSink *sink, BrowseSessionController *currentItems, HomeModelController *home,
        ContentModelController *content, SearchController *search, QObject *parent = nullptr);

    void applyResumeTicks(const QString& itemId, qint64 positionTicks);
    void applyFavorite(const QString& itemId, bool favorite);
    void applyPlayed(const QString& itemId, bool played);
    void recordPlaybackStopped(const MovieItem& item, const QString& itemId, qint64 positionTicks, bool watched,
        const MovieItem& successor, quint64 reportId);
    // The reporter invokes this after draining the ending session's reports.
    void persistPlaybackWatched(const QString& itemId, quint64 reportId);
    void reset();
    Q_INVOKABLE void setFavorite(const QString& itemId, bool favorite);
    Q_INVOKABLE void setPlayed(const QString& itemId, bool played);
    Q_INVOKABLE void clearProgress(const QString& itemId);

signals:
    void favoriteChanged(const QString& itemId, bool favorite);
    void playedChanged(const QString& itemId, bool played);
    void errorOccurred(const QString& message);

private:
    enum class WriteKind { Favorite, Played, ClearProgress };
    struct PendingWrite {
        WriteKind kind;
        bool value;
        bool publish;
        RequestGeneration::Token playbackRevision;
    };
    struct ItemWrites {
        QQueue<PendingWrite> requests;
        RequestGeneration playbackRevision;
    };

    void enqueueWrite(const QString& itemId, WriteKind kind, bool value, bool publish = true);
    void startNextWrite(const QString& itemId);
    void finishWrite(const QString& itemId, const PendingWrite& write, RequestGeneration::Token generation,
        const std::exception_ptr& error = {});

    UserItemStateSink *m_api = nullptr;
    BrowseSessionController *m_browse = nullptr;
    HomeModelController *m_home = nullptr;
    ContentModelController *m_content = nullptr;
    SearchController *m_search = nullptr;
    // Pending operations only, not an item-state cache. An explicit mutation
    // cancels a deferred playback completion before its server write.
    QHash<QString, quint64> m_pendingPlaybackWatched;
    // Manual writes serialize per item and publish only after server success.
    QHash<QString, ItemWrites> m_pendingWrites;
    RequestGeneration m_writeGeneration;
};

} // namespace Spool
