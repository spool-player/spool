#pragma once

#include <QObject>
#include <QString>

#include <memory>

namespace Spool {

struct RemoteMediaSessionState {
    QString title;
    QString artist;
    QString album;
    QString targetName;
    qint64 durationMs = 0;
    qint64 positionMs = 0;
    double playbackRate = 1.0;
    int volume = 100;
    bool playing = false;
    bool canSeek = false;
};

class PlatformRemoteMediaSession : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~PlatformRemoteMediaSession() override = default;

    virtual void update(const RemoteMediaSessionState& state) = 0;
    virtual void clear() = 0;

signals:
    void playRequested();
    void pauseRequested();
    void playPauseRequested();
    void stopRequested();
    void nextRequested();
    void previousRequested();
    void seekRequested(qint64 positionMs);
    void seekRelativeRequested(qint64 deltaMs);
    void volumeRequested(int volume);
};

std::unique_ptr<PlatformRemoteMediaSession> createPlatformRemoteMediaSession(QObject *parent = nullptr);

} // namespace Spool
