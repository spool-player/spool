#pragma once

#include <QObject>
#include <QPointer>

class QQuickImageResponse;

#include <optional>

namespace Spool {

class ApplicationHooks;
class PlayerController;
class PlayQueueController;

// Android owns the system controls; mpv remains the sole playback engine.
class AndroidLocalMediaSession final : public QObject {
public:
    explicit AndroidLocalMediaSession(ApplicationHooks& hooks);
    ~AndroidLocalMediaSession() override;

    void control(int action, qint64 value);

private:
    void update();
    void clear();
    void setPaused(bool paused);
    void updateArtwork(const QString& url);

    ApplicationHooks& m_hooks;
    PlayerController& m_player;
    PlayQueueController& m_queue;
    bool m_active = false;
    std::optional<bool> m_transitionPaused;
    QString m_artworkUrl;
    QPointer<QQuickImageResponse> m_artworkResponse;
};

} // namespace Spool
