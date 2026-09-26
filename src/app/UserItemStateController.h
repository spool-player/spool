#pragma once

#include "../provider/UserItemStateSink.h"

#include <QObject>
#include <QString>

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
    void recordPlaybackStopped(const MovieItem& item, const QString& itemId, qint64 positionTicks, bool completed);
    Q_INVOKABLE void setFavorite(const QString& itemId, bool favorite);
    Q_INVOKABLE void setPlayed(const QString& itemId, bool played);
    Q_INVOKABLE void clearProgress(const QString& itemId);

signals:
    void favoriteChanged(const QString& itemId, bool favorite);
    void playedChanged(const QString& itemId, bool played);
    void errorOccurred(const QString& message);

private:
    UserItemStateSink *m_api = nullptr;
    BrowseSessionController *m_browse = nullptr;
    HomeModelController *m_home = nullptr;
    ContentModelController *m_content = nullptr;
    SearchController *m_search = nullptr;
};

} // namespace Spool
