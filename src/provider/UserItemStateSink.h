#pragma once

#include <QCoroTask>

#include <QString>

namespace Spool {

// Where per-user item state is written back: played, favourite and resume
// position. Plain abstract class, see Catalog. A source that keeps no such
// state completes each call without doing anything.
class UserItemStateSink {
public:
    virtual ~UserItemStateSink() = default;

    virtual bool signedIn() const = 0;
    virtual QCoro::Task<void> setItemFavorite(QString itemId, bool favorite) = 0;
    virtual QCoro::Task<void> setItemPlayed(QString itemId, bool played) = 0;
    virtual QCoro::Task<void> setItemPlaybackPosition(QString itemId, qint64 positionTicks) = 0;
};

} // namespace Spool
