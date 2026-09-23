#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class QGuiApplication;

namespace JellyfinNative {

class ArtworkService;
class NativeAppWindow;
class PlayQueueController;
class PlayerController;
class RouterController;
class SettingsController;

// What the platform layer needs from the application above it, handed down
// at startup so nothing under src/platform names the composition root. The
// signals are forwarded from the app; the callback runs on the GUI thread.
class ApplicationHooks final : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    PlayerController *player = nullptr;
    SettingsController *settings = nullptr;
    PlayQueueController *playQueue = nullptr;
    ArtworkService *artwork = nullptr;
    // A platform memory warning at the given level ("low" or "critical").
    std::function<void(const QString& level)> memoryPressure;
    // Local playback, for the system media controls.
    std::function<bool()> playbackTransition;
    std::function<void()> stopPlayback;
    std::function<void()> playNext;
    std::function<void()> playPrevious;

signals:
    // The app is between one queue item and the next.
    void playbackTransitionChanged();
    // The app has shed what it can; the window may drop its resources too.
    void aggressiveMemoryPressure();
    void diagnosticsReportSaved(const QString& path);
    void toastRequested(const QString& message);
};

class PlatformApplicationServices final {
public:
    PlatformApplicationServices(
        QGuiApplication& application, NativeAppWindow& window, ApplicationHooks& hooks, RouterController& router);
    ~PlatformApplicationServices();

    PlatformApplicationServices(const PlatformApplicationServices&) = delete;
    PlatformApplicationServices& operator=(const PlatformApplicationServices&) = delete;

    void start();

private:
    struct PlatformData;
    std::unique_ptr<PlatformData> m_platform;
};

} // namespace JellyfinNative
