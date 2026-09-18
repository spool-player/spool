#pragma once

#include <QObject>

#include <optional>

namespace JellyfinNative {

class AppController;
class PlayerController;
class PlayQueueController;

// Android owns the system controls; mpv remains the sole playback engine.
class AndroidLocalMediaSession final : public QObject {
public:
    explicit AndroidLocalMediaSession(AppController& controller);
    ~AndroidLocalMediaSession() override;

    void control(int action, qint64 value);

private:
    void update();
    void clear();
    void setPaused(bool paused);

    AppController& m_controller;
    PlayerController& m_player;
    PlayQueueController& m_queue;
    bool m_active = false;
    std::optional<bool> m_transitionPaused;
};

} // namespace JellyfinNative
