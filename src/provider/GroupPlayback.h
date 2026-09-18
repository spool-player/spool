#pragma once

#include <QCoroTask>

#include <QObject>
#include <QString>
#include <QStringList>

namespace JellyfinNative {

// Watching together: while a group is joined, the queue and the transport
// belong to the group, and the app hands every local action here instead of
// acting on the player. This is the surface the app drives; how the group is
// created, joined and shown is the provider's own QML.
class GroupPlayback : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // Whether a group is joined, and therefore whether the app must route
    // playback actions through it.
    virtual bool enabled() const = 0;

    virtual void requestTogglePause() = 0;
    virtual void requestSeek(double positionSeconds) = 0;
    virtual void requestRelativeSeek(double deltaSeconds) = 0;
    virtual void requestNextItem() = 0;
    virtual void requestPreviousItem() = 0;
    virtual void requestPlayItem(const QString& playlistItemId) = 0;
    virtual void requestMoveItem(const QString& playlistItemId, int newIndex) = 0;
    virtual void requestRemoveItems(const QStringList& playlistItemIds) = 0;
    virtual void requestQueueItems(const QStringList& itemIds, bool queueNext) = 0;
    // Replaces the group's queue with the local one and asks it to start at
    // `playingIndex`. Arm requestUnpauseWhenReady() before calling: the
    // group's own queue update can land before or after the reply.
    virtual QCoro::Task<void> publishQueue(QStringList itemIds, int playingIndex, qint64 startPositionTicks) = 0;
    virtual void requestUnpauseWhenReady() = 0;
    virtual void cancelPendingUnpause() = 0;
    virtual void joinGroup(const QString& groupId) = 0;
    virtual void leaveGroup() = 0;

signals:
    // The group wants this client to start what its queue points at, from
    // this position.
    void queuePlaybackRequested(qint64 positionTicks);
};

} // namespace JellyfinNative
