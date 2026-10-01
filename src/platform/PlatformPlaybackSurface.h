#pragma once

#include "player/MpvOptionProfile.h"

#include <QString>

#include <functional>

class QObject;
struct mpv_handle;

namespace Spool {

class NativeAppWindow;
struct PlaybackSession;

bool platformIdleMpvPreparationEnabled();
void runAfterPlatformMpvLoaded(std::function<void()> callback);
MpvOptionProfile::Platform platformMpvOptionProfile();
// Whether video goes through the Qt scene graph. `directRequested` carries
// the viewer's choice for platforms that offer one; the rest ignore it,
// because what they can do is decided by the file, not by a setting.
bool platformUsesEmbeddedVideo(const PlaybackSession& session, bool directRequested);
QString platformPlaybackBackendName(bool embeddedVideo);

bool configurePlatformMpvSurface(
    mpv_handle *handle, NativeAppWindow& window, bool needsVideoSurface, bool embeddedVideo, QString& errorMessage);
bool attachPlatformMpvSurface(mpv_handle *handle, bool needsVideoSurface, bool embeddedVideo, QObject& context,
    std::function<void(const QString&)> errorHandler, QString& errorMessage);
bool waitForPlatformMpvSurfaceReady(bool needsVideoSurface, bool embeddedVideo, QString& errorMessage);
bool releasePlatformMpvSurface(bool embeddedVideo);
QString platformPreparingStatus(bool needsVideoSurface, bool embeddedVideo);
bool applyPlatformSubtitlePreload(
    mpv_handle *handle, const PlaybackSession& session, const QString& preferredLanguage, QString& errorMessage);

bool platformUsesBackgroundPlaybackPolicy();
void platformAudioTrackChanged(int index);
// The video's display size, once mpv knows it. A platform that hands the
// decoder a surface has to shape that surface itself, because the decoder
// fills whatever it is given and a mismatch stretches the picture. Ignored
// where the scene graph already letterboxes.
void platformVideoSizeChanged(int width, int height);

} // namespace Spool
