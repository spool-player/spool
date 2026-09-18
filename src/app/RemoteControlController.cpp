#include "RemoteControlController.h"
#include "SpoolRemoteProtocol.h"

#include "../api/JellyfinApiFacade.h"
#include "../common/AsyncTask.h"
#include "../common/MetaJson.h"
#include "../platform/PlatformRemoteMediaSession.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonValue>

#include <algorithm>
#include <cstdlib>

namespace JellyfinNative {
namespace {

    // How far the target's own report may sit from where a seek put it before
    // the reading is treated as confirmation. One poll interval of playback
    // plus room for the report itself being a moment stale.
    constexpr qint64 kSeekConfirmToleranceTicks = 25'000'000;
    // How long to keep holding an unconfirmed seek. Past this the target is
    // assumed to have ignored it and its own report wins again.
    constexpr qint64 kSeekConfirmTimeoutMs = 8'000;

    qint64 jsonInteger(const QJsonValue& value)
    {
        return value.toVariant().toLongLong();
    }

    QString streamLabel(const QJsonObject& stream)
    {
        QString label = stream.value(QStringLiteral("DisplayTitle")).toString().trimmed();
        if (!label.isEmpty())
            return label;
        QStringList parts;
        const QString language = stream.value(QStringLiteral("Language")).toString().trimmed();
        const QString codec = stream.value(QStringLiteral("Codec")).toString().trimmed().toUpper();
        if (!language.isEmpty())
            parts.push_back(language);
        if (!codec.isEmpty())
            parts.push_back(codec);
        return parts.isEmpty() ? QStringLiteral("Track %1").arg(stream.value(QStringLiteral("Index")).toInt() + 1)
                               : parts.join(QStringLiteral(" · "));
    }

    QVariantMap normalizedNowPlayingItem(const QJsonObject& object)
    {
        if (object.isEmpty())
            return {};
        return metaToJson(metaFromJson<MovieItem>(object, MetaJsonKeyPolicy::PascalCase)).toVariantMap();
    }

} // namespace

RemoteControlController::RemoteControlController(JellyfinApiFacade *api, QObject *parent)
    : RemotePlayback(parent)
    , m_api(api)
    , m_mediaSession(createPlatformRemoteMediaSession(this))
{
    m_link = new SpoolLink(api, this);
    m_refreshTimer.setInterval(5'000);
    connect(&m_refreshTimer, &QTimer::timeout, this, &RemoteControlController::refreshTargets);
    m_pendingRefreshTimer.setInterval(1'000);
    connect(&m_pendingRefreshTimer, &QTimer::timeout, this, &RemoteControlController::refreshTargets);
    m_pendingTimeoutTimer.setSingleShot(true);
    m_pendingTimeoutTimer.setInterval(30'000);
    connect(&m_pendingTimeoutTimer, &QTimer::timeout, this, [this]() {
        const QString targetName = m_selectedTargetName;
        m_playGeneration.invalidate();
        setPlaybackPending(false);
        refreshTargets();
        emit feedbackText(QStringLiteral("Playback did not start on %1").arg(targetName));
    });

    connect(m_mediaSession.get(), &PlatformRemoteMediaSession::playRequested, this, [this]() { requestPaused(false); });
    connect(m_mediaSession.get(), &PlatformRemoteMediaSession::pauseRequested, this, [this]() { requestPaused(true); });
    connect(m_mediaSession.get(), &PlatformRemoteMediaSession::playPauseRequested, this,
        &RemoteControlController::togglePause);
    connect(
        m_mediaSession.get(), &PlatformRemoteMediaSession::stopRequested, this, &RemoteControlController::stopPlayback);
    connect(
        m_mediaSession.get(), &PlatformRemoteMediaSession::nextRequested, this, &RemoteControlController::nextTrack);
    connect(m_mediaSession.get(), &PlatformRemoteMediaSession::previousRequested, this,
        &RemoteControlController::previousTrack);
    connect(m_mediaSession.get(), &PlatformRemoteMediaSession::seekRequested, this,
        [this](qint64 positionMs) { seek(positionMs * 10'000); });
    connect(m_mediaSession.get(), &PlatformRemoteMediaSession::seekRelativeRequested, this,
        [this](qint64 deltaMs) { seekRelative(deltaMs * 10'000); });
    connect(
        m_mediaSession.get(), &PlatformRemoteMediaSession::volumeRequested, this, &RemoteControlController::setVolume);
}

RemoteControlController::~RemoteControlController() = default;

void RemoteControlController::start()
{
    if (!m_refreshTimer.isActive())
        m_refreshTimer.start();
    refreshTargets();
}

void RemoteControlController::stop()
{
    m_refreshTimer.stop();
    m_sessionsGeneration.invalidate();
    m_sessions.clear();
    m_targets.clear();
    clearTarget();
    emit targetsChanged();
}

void RemoteControlController::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}
void RemoteControlController::setPlaybackPending(bool pending)
{
    if (m_playbackPending == pending)
        return;
    m_playbackPending = pending;
    if (pending) {
        m_pendingTimeoutTimer.start();
    } else {
        m_pendingRefreshTimer.stop();
        m_pendingTimeoutTimer.stop();
        m_pendingTargetSessionId.clear();
        m_pendingItemId.clear();
        m_pendingPreviousItemId.clear();
        m_pendingTitle.clear();
    }
    emit playbackPendingChanged();
}

void RemoteControlController::refreshTargets()
{
    if (!m_api || m_api->session().accessToken.isEmpty() || m_busy)
        return;
    const RequestGeneration::Token generation = m_sessionsGeneration.current();
    setBusy(true);
    Async::runScoped(
        this, m_api->fetchControllableSessions(),
        [this, generation](const QJsonArray& sessions) {
            setBusy(false);
            if (m_sessionsGeneration.isCurrent(generation))
                applySessions(sessions);
        },
        [this](const std::exception_ptr& error) {
            setBusy(false);
            reportCommandError(QStringLiteral("refresh remote targets"), error);
        },
        "remote target refresh");
}

void RemoteControlController::applySessions(const QJsonArray& sessions)
{
    // A pushed session snapshot supersedes an HTTP refresh already in flight.
    m_sessionsGeneration.invalidate();
    QHash<QString, QJsonObject> nextSessions;
    QVariantList nextTargets;
    const QString localDeviceId = m_api ? m_api->deviceId() : QString();
    QString localSessionId;

    for (const QJsonValue& value : sessions) {
        const QJsonObject session = value.toObject();
        const QString id = session.value(QStringLiteral("Id")).toString();
        if (!localDeviceId.isEmpty() && session.value(QStringLiteral("DeviceId")).toString() == localDeviceId)
            localSessionId = id;
        if (id.isEmpty() || session.value(QStringLiteral("DeviceId")).toString() == localDeviceId)
            continue;
        const QJsonObject capabilities = session.value(QStringLiteral("Capabilities")).toObject();
        if (!capabilities.value(QStringLiteral("SupportsMediaControl")).toBool(false))
            continue;

        nextSessions.insert(id, session);
        const QJsonObject nowPlaying = session.value(QStringLiteral("NowPlayingItem")).toObject();
        nextTargets.push_back(QVariantMap {
            { QStringLiteral("sessionId"), id },
            { QStringLiteral("deviceName"), session.value(QStringLiteral("DeviceName")).toString() },
            { QStringLiteral("deviceType"), session.value(QStringLiteral("DeviceType")).toString() },
            { QStringLiteral("client"), session.value(QStringLiteral("Client")).toString() },
            { QStringLiteral("userName"), session.value(QStringLiteral("UserName")).toString() },
            { QStringLiteral("nowPlayingTitle"), nowPlaying.value(QStringLiteral("Name")).toString() },
            { QStringLiteral("appVersion"), session.value(QStringLiteral("ApplicationVersion")).toString() },
        });
    }

    std::sort(nextTargets.begin(), nextTargets.end(), [](const QVariant& left, const QVariant& right) {
        return QString::localeAwareCompare(left.toMap().value(QStringLiteral("deviceName")).toString(),
                   right.toMap().value(QStringLiteral("deviceName")).toString())
            < 0;
    });

    if (!localSessionId.isEmpty() && localSessionId != m_localSessionId) {
        m_localSessionId = localSessionId;
        m_link->setLocalSessionId(localSessionId);
        // A peer can only answer once it has somewhere to answer to.
        if (!m_selectedSessionId.isEmpty())
            m_link->connectToPeer(m_selectedSessionId);
    }
    const bool targetListChanged = m_targets != nextTargets;
    m_sessions = std::move(nextSessions);
    m_targets = std::move(nextTargets);
    if (targetListChanged) {
        qInfo() << "remote control: discovered" << m_targets.size() << "controllable client(s)";
        emit targetsChanged();
    }

    if (!m_selectedSessionId.isEmpty() && !m_sessions.contains(m_selectedSessionId)) {
        clearTarget();
        return;
    }
    if (!m_selectedSessionId.isEmpty())
        applySelectedSession();
}

void RemoteControlController::selectTarget(const QString& sessionId)
{
    const QString normalized = sessionId.trimmed();
    if (!m_sessions.contains(normalized)) {
        emit errorText(QStringLiteral("That remote client is no longer available."));
        refreshTargets();
        return;
    }
    if (m_selectedSessionId == normalized) {
        applySelectedSession();
        return;
    }
    clearSelectedState(false);
    m_selectedSessionId = normalized;
    m_link->connectToPeer(normalized);
    applySelectedSession(true);
}

void RemoteControlController::clearTarget()
{
    if (m_selectedSessionId.isEmpty() && m_selectedTargetName.isEmpty()) {
        clearSelectedState();
        return;
    }
    m_link->reset();
    m_selectedSessionId.clear();
    m_selectedTargetName.clear();
    m_selectedTargetDetail.clear();
    clearSelectedState();
    emit targetChanged();
}

void RemoteControlController::clearPendingCommands()
{
    m_pauseGeneration.invalidate();
    m_pendingPaused.reset();
    m_pendingPauseDeadlineMs = 0;
    m_pendingSeekTicks.reset();
    m_pendingSeekAtMs = 0;
    m_pendingSeekDeadlineMs = 0;
}

void RemoteControlController::clearSelectedState(bool notify)
{
    m_queueGeneration.invalidate();
    m_playGeneration.invalidate();
    clearPendingCommands();
    setPlaybackPending(false);
    m_trickplayGeneration.invalidate();
    m_trickplayTimeline.clear();
    m_trickplayItemId.clear();
    m_trickplayMediaSourceId.clear();
    m_previewSentTicks = -1;
    m_previewSentActive = false;
    if (notify)
        emit trickplayChanged();
    m_nowPlayingItem.clear();
    m_queue.clear();
    m_audioTracks.clear();
    m_subtitleTracks.clear();
    m_supportedCommands.clear();
    m_rawQueue = {};
    m_positionTicks = 0;
    m_runtimeTicks = 0;
    m_stateReceivedAtMs = 0;
    m_playbackRate = 1.0;
    m_volume = 100;
    m_paused = true;
    m_muted = false;
    m_shuffled = false;
    m_repeatMode = QStringLiteral("RepeatNone");
    if (notify) {
        if (m_mediaSession)
            m_mediaSession->clear();
        emit stateChanged();
    }
}

void RemoteControlController::applySelectedSession(bool selectionChanged)
{
    const QJsonObject session = m_sessions.value(m_selectedSessionId);
    if (session.isEmpty())
        return;

    const QString previousTargetName = m_selectedTargetName;
    const QString previousTargetDetail = m_selectedTargetDetail;
    m_selectedTargetName = session.value(QStringLiteral("DeviceName")).toString().trimmed();
    if (m_selectedTargetName.isEmpty())
        m_selectedTargetName = session.value(QStringLiteral("Client")).toString().trimmed();
    const QString client = session.value(QStringLiteral("Client")).toString().trimmed();
    const QString user = session.value(QStringLiteral("UserName")).toString().trimmed();
    QStringList detailParts;
    if (!client.isEmpty())
        detailParts.push_back(client);
    if (!user.isEmpty())
        detailParts.push_back(user);
    m_selectedTargetDetail = detailParts.join(QStringLiteral(" · "));
    const bool targetInfoChanged
        = previousTargetName != m_selectedTargetName || previousTargetDetail != m_selectedTargetDetail;

    const QJsonObject nowPlaying = session.value(QStringLiteral("NowPlayingItem")).toObject();
    const QJsonObject playState = session.value(QStringLiteral("PlayState")).toObject();
    if (m_playbackPending && m_selectedSessionId == m_pendingTargetSessionId) {
        const QString nowPlayingId = nowPlaying.value(QStringLiteral("Id")).toString();
        // Empty while the target is between items, which is most of what a
        // start looks like from here.
        const bool unchanged = nowPlayingId.isEmpty() || nowPlayingId == m_pendingPreviousItemId;
        if (nowPlayingId == m_pendingItemId) {
            setPlaybackPending(false);
        } else if (unchanged) {
            // The command has not landed yet. Hold the item the user chose
            // rather than flicking back to the one being replaced.
            if (targetInfoChanged)
                emit targetChanged();
            emit stateChanged();
            return;
        } else {
            // Playing something nobody here asked for: the target was driven
            // directly, so follow it now instead of showing a stale item for
            // the rest of the timeout.
            setPlaybackPending(false);
        }
    }
    if (nowPlaying.value(QStringLiteral("Id")).toString()
        != m_nowPlayingItem.value(QStringLiteral("movieId")).toString())
        clearPendingCommands();
    m_nowPlayingItem = normalizedNowPlayingItem(nowPlaying);
    loadTrickplay(nowPlaying.value(QStringLiteral("Id")).toString(),
        nowPlaying.value(QStringLiteral("MediaSourceId")).toString());
    const qint64 reportedTicks = jsonInteger(playState.value(QStringLiteral("PositionTicks")));
    m_runtimeTicks = jsonInteger(nowPlaying.value(QStringLiteral("RunTimeTicks")));
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    m_playbackRate = playState.value(QStringLiteral("PlaybackRate")).toDouble(1.0);
    if (m_pendingSeekTicks && nowMs <= m_pendingSeekDeadlineMs) {
        // Hold the position this device asked for until the target's own
        // report reaches it, so the bar stays where the user put it.
        const qint64 elapsedTicks
            = m_paused ? 0 : static_cast<qint64>((nowMs - m_pendingSeekAtMs) * 10'000.0 * m_playbackRate);
        const qint64 expected = *m_pendingSeekTicks + elapsedTicks;
        if (std::llabs(reportedTicks - expected) <= kSeekConfirmToleranceTicks) {
            m_pendingSeekTicks.reset();
            m_pendingSeekDeadlineMs = 0;
            m_positionTicks = reportedTicks;
            m_stateReceivedAtMs = nowMs;
        } else {
            m_positionTicks = *m_pendingSeekTicks;
            m_stateReceivedAtMs = m_pendingSeekAtMs;
        }
    } else {
        m_pendingSeekTicks.reset();
        m_pendingSeekDeadlineMs = 0;
        m_positionTicks = reportedTicks;
        m_stateReceivedAtMs = nowMs;
    }
    const bool reportedPaused = playState.value(QStringLiteral("IsPaused")).toBool(true);
    if (m_pendingPaused && nowMs <= m_pendingPauseDeadlineMs) {
        if (reportedPaused == *m_pendingPaused) {
            m_pendingPaused.reset();
            m_pendingPauseDeadlineMs = 0;
        } else
            m_paused = *m_pendingPaused;
    } else {
        m_pendingPauseDeadlineMs = 0;
        m_pendingPaused.reset();
        m_paused = reportedPaused;
    }
    m_muted = playState.value(QStringLiteral("IsMuted")).toBool(false);
    m_volume = std::clamp(playState.value(QStringLiteral("VolumeLevel")).toInt(100), 0, 100);
    m_repeatMode = playState.value(QStringLiteral("RepeatMode")).toString(QStringLiteral("RepeatNone"));
    m_shuffled = playState.value(QStringLiteral("PlaybackOrder")).toString() == QStringLiteral("Shuffle");

    const QJsonArray commands
        = session.value(QStringLiteral("Capabilities")).toObject().value(QStringLiteral("SupportedCommands")).toArray();
    m_supportedCommands.clear();
    m_supportedCommands.reserve(commands.size());
    for (const QJsonValue& command : commands) {
        const QString name = command.toString();
        if (!name.isEmpty())
            m_supportedCommands.push_back(name);
    }

    const int selectedAudio = playState.value(QStringLiteral("AudioStreamIndex")).toInt(-1);
    const int selectedSubtitle = playState.value(QStringLiteral("SubtitleStreamIndex")).toInt(-1);
    m_audioTracks.clear();
    m_subtitleTracks.clear();
    for (const QJsonValue& value : nowPlaying.value(QStringLiteral("MediaStreams")).toArray()) {
        const QJsonObject stream = value.toObject();
        const int index = stream.value(QStringLiteral("Index")).toInt(-1);
        QVariantMap row {
            { QStringLiteral("index"), index },
            { QStringLiteral("label"), streamLabel(stream) },
            { QStringLiteral("selected"), false },
        };
        if (stream.value(QStringLiteral("Type")).toString() == QStringLiteral("Audio")) {
            row.insert(QStringLiteral("selected"), index == selectedAudio);
            m_audioTracks.push_back(row);
        } else if (stream.value(QStringLiteral("Type")).toString() == QStringLiteral("Subtitle")) {
            row.insert(QStringLiteral("selected"), index == selectedSubtitle);
            m_subtitleTracks.push_back(row);
        }
    }
    if (!m_subtitleTracks.isEmpty()) {
        m_subtitleTracks.prepend(QVariantMap {
            { QStringLiteral("index"), -1 },
            { QStringLiteral("label"), QStringLiteral("Off") },
            { QStringLiteral("selected"), selectedSubtitle < 0 },
        });
    }

    m_rawQueue = session.value(QStringLiteral("NowPlayingQueue")).toArray();
    refreshQueueDetails(m_rawQueue);
    updateMediaSession();
    if (selectionChanged)
        emit trickplayChanged();
    if (selectionChanged || targetInfoChanged)
        emit targetChanged();
    emit stateChanged();
}

void RemoteControlController::refreshQueueDetails(const QJsonArray& rawQueue)
{
    const RequestGeneration::Token generation = m_queueGeneration.next();
    if (rawQueue.isEmpty()) {
        m_queue.clear();
        emit stateChanged();
        return;
    }

    QStringList ids;
    ids.reserve(rawQueue.size());
    for (const QJsonValue& value : rawQueue) {
        const QString id = value.toObject().value(QStringLiteral("Id")).toString();
        if (!id.isEmpty())
            ids.push_back(id);
    }
    if (ids.isEmpty() || !m_api) {
        m_queue.clear();
        emit stateChanged();
        return;
    }

    const QJsonObject selectedSession = m_sessions.value(m_selectedSessionId);
    const QString currentPlaylistItemId = selectedSession.value(QStringLiteral("PlaylistItemId")).toString();
    const QString currentItemId
        = selectedSession.value(QStringLiteral("NowPlayingItem")).toObject().value(QStringLiteral("Id")).toString();
    Async::runScoped(
        this, m_api->fetchItemsByIds(ids),
        [this, generation, rawQueue, currentPlaylistItemId, currentItemId](const std::vector<MovieItem>& items) {
            if (generation != m_queueGeneration.current())
                return;
            QVariantList queue;
            queue.reserve(rawQueue.size());
            for (const QJsonValue& value : rawQueue) {
                const QJsonObject raw = value.toObject();
                const QString id = raw.value(QStringLiteral("Id")).toString();
                const auto item = std::find_if(
                    items.cbegin(), items.cend(), [&id](const MovieItem& candidate) { return candidate.id == id; });
                QVariantMap row = item == items.cend() ? QVariantMap {} : metaToJson(*item).toVariantMap();
                row.insert(QStringLiteral("movieId"), id);
                row.insert(QStringLiteral("playlistItemId"), raw.value(QStringLiteral("PlaylistItemId")).toString());
                const QString playlistItemId = raw.value(QStringLiteral("PlaylistItemId")).toString();
                row.insert(QStringLiteral("current"),
                    !currentPlaylistItemId.isEmpty() ? playlistItemId == currentPlaylistItemId : id == currentItemId);
                queue.push_back(row);
            }
            m_queue = std::move(queue);
            emit stateChanged();
        },
        [this, generation](const std::exception_ptr& error) {
            if (generation != m_queueGeneration.current())
                return;
            reportCommandError(QStringLiteral("load remote queue"), error);
        },
        "remote queue details");
}

void RemoteControlController::loadTrickplay(const QString& itemId, const QString& mediaSourceId)
{
    const QString normalizedMediaSourceId = mediaSourceId.trimmed();
    if (itemId == m_trickplayItemId
        && (normalizedMediaSourceId.isEmpty() || normalizedMediaSourceId == m_trickplayMediaSourceId))
        return;

    m_trickplayItemId = itemId;
    m_trickplayMediaSourceId = normalizedMediaSourceId;
    m_trickplayTimeline.clear();
    emit trickplayChanged();
    const RequestGeneration::Token generation = m_trickplayGeneration.next();
    if (!m_api || itemId.isEmpty())
        return;

    Async::runScoped(
        this, m_api->fetchTrickplayInfo(itemId, normalizedMediaSourceId),
        [this, generation, itemId](const TrickplayInfo& trickplay) {
            if (!m_trickplayGeneration.isCurrent(generation) || m_trickplayItemId != itemId)
                return;
            PlaybackSession session;
            session.itemId = itemId;
            session.trickplay = trickplay;
            m_trickplayTimeline.setSession(session);
            emit trickplayChanged();
        },
        [this, generation, itemId](const std::exception_ptr& error) {
            if (!m_trickplayGeneration.isCurrent(generation) || m_trickplayItemId != itemId)
                return;
            qInfo() << "remote trickplay: metadata unavailable for" << itemId << ":" << exceptionMessage(error);
        },
        "remote trickplay metadata");
}

QVariantMap RemoteControlController::trickplayForTicks(qint64 positionTicks) const
{
    QVariantMap result;
    if (!m_api || !m_trickplayTimeline.trickplayAvailable()) {
        result.insert(QStringLiteral("available"), false);
        return result;
    }

    const PlaybackTimeline::TrickplayFrame frame
        = m_trickplayTimeline.trickplayFrameAt(std::max<qint64>(0, positionTicks) / 10'000'000.0);
    if (!frame.available) {
        result.insert(QStringLiteral("available"), false);
        return result;
    }
    result.insert(QStringLiteral("available"), true);
    result.insert(QStringLiteral("url"),
        m_api->trickplayTileUrl(m_trickplayItemId, m_trickplayTimeline.trickplayWidth(), frame.sheetIndex));
    result.insert(QStringLiteral("width"), frame.width);
    result.insert(QStringLiteral("height"), frame.height);
    result.insert(QStringLiteral("offsetX"), frame.offsetX);
    result.insert(QStringLiteral("offsetY"), frame.offsetY);
    result.insert(QStringLiteral("sheetWidth"), frame.sheetWidth);
    result.insert(QStringLiteral("sheetHeight"), frame.sheetHeight);
    return result;
}

void RemoteControlController::previewSeek(qint64 positionTicks, bool active)
{
    if (!targetSelected() || !m_link || m_trickplayItemId.isEmpty())
        return;
    const qint64 ticks = std::max<qint64>(0, positionTicks);
    if (ticks == m_previewSentTicks && active == m_previewSentActive)
        return;
    m_previewSentTicks = ticks;
    m_previewSentActive = active;
    // Only the newest position is worth showing, so an older one still
    // waiting for the link is replaced rather than queued behind it. The
    // teardown is not: it is what puts the other screen back.
    m_link->send(m_selectedSessionId, SpoolRemoteProtocol::seekPreviewMessage(m_trickplayItemId, ticks, active),
        active ? SpoolLink::Delivery::Coalesced : SpoolLink::Delivery::Reliable);
}

void RemoteControlController::cancelSeekPreview()
{
    if (m_previewSentActive)
        previewSeek(m_previewSentTicks, false);
}

void RemoteControlController::requestSyncPlayJoin(const QString& groupId)
{
    if (!targetSelected() || !m_link || groupId.isEmpty())
        return;
    m_link->send(m_selectedSessionId, SpoolRemoteProtocol::syncPlayJoinMessage(groupId));
}

bool RemoteControlController::supports(const QString& command) const
{
    return m_supportedCommands.contains(command);
}

qint64 RemoteControlController::predictedPositionTicks() const
{
    if (m_paused || m_positionTicks <= 0 || m_stateReceivedAtMs <= 0)
        return m_positionTicks;
    const qint64 elapsedMs = std::max<qint64>(0, QDateTime::currentMSecsSinceEpoch() - m_stateReceivedAtMs);
    const qint64 predicted = m_positionTicks + static_cast<qint64>(elapsedMs * 10'000.0 * m_playbackRate);
    return m_runtimeTicks > 0 ? std::min(predicted, m_runtimeTicks) : predicted;
}

void RemoteControlController::stagePendingPlayback(
    const std::vector<MovieItem>& items, qint64 startPositionTicks, const QString& command)
{
    if (command != QStringLiteral("PlayNow") && command != QStringLiteral("PlayShuffle"))
        return;

    QVariantList queue;
    QJsonArray rawQueue;
    QVariantMap selectedItem;
    queue.reserve(static_cast<qsizetype>(items.size()));
    for (int index = 0; index < static_cast<int>(items.size()); ++index) {
        const MovieItem& item = items[static_cast<size_t>(index)];
        if (item.id.isEmpty() || !isPlayableItem(item))
            continue;
        QVariantMap row = metaToJson(item).toVariantMap();
        row.insert(QStringLiteral("movieId"), item.id);
        row.insert(QStringLiteral("current"), item.id == m_pendingItemId);
        queue.push_back(row);
        rawQueue.append(QJsonObject { { QStringLiteral("Id"), item.id } });
        if (item.id == m_pendingItemId)
            selectedItem = row;
    }
    if (selectedItem.isEmpty() && !queue.isEmpty())
        selectedItem = queue.front().toMap();

    m_queueGeneration.invalidate();
    m_nowPlayingItem = selectedItem;
    m_queue = std::move(queue);
    m_rawQueue = std::move(rawQueue);
    m_audioTracks.clear();
    m_subtitleTracks.clear();
    m_positionTicks = std::max<qint64>(0, startPositionTicks);
    loadTrickplay(selectedItem.value(QStringLiteral("movieId")).toString());
    m_runtimeTicks = selectedItem.value(QStringLiteral("runtimeTicks")).toLongLong();
    m_stateReceivedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_playbackRate = 1.0;
    m_paused = false;
    updateMediaSession();
    emit stateChanged();
}

bool RemoteControlController::playItems(
    const std::vector<MovieItem>& items, int startIndex, const QString& command, bool fromStart)
{
    if (!targetSelected())
        return false;
    QStringList ids;
    int remoteStartIndex = -1;
    qint64 startPositionTicks = -1;
    QString title;
    for (int index = 0; index < static_cast<int>(items.size()); ++index) {
        const MovieItem& item = items[static_cast<size_t>(index)];
        if (item.id.isEmpty() || !isPlayableItem(item))
            continue;
        if (index == startIndex) {
            remoteStartIndex = ids.size();
            startPositionTicks = fromStart ? 0 : std::max<qint64>(0, item.resumeTicks);
            title = item.title;
        }
        ids.push_back(item.id);
    }
    if (ids.isEmpty()) {
        emit errorText(QStringLiteral("This list has no remotely playable items."));
        return true;
    }
    if (remoteStartIndex < 0)
        remoteStartIndex = 0;
    // The result is the caller's, not swallowed: every caller treats a true
    // here as "handled remotely, do not play locally", so reporting success
    // for a command that never went out strands the user with nothing playing
    // anywhere.
    if (!runPlay(ids, command, remoteStartIndex, startPositionTicks, title))
        return false;
    stagePendingPlayback(items, startPositionTicks, command);
    return true;
}

void RemoteControlController::beginPendingPlayback(const QString& target, const QString& itemId, const QString& title)
{
    clearPendingCommands();
    m_pendingPreviousItemId = m_nowPlayingItem.value(QStringLiteral("movieId")).toString();
    m_pendingTargetSessionId = target;
    m_pendingItemId = itemId;
    m_pendingTitle = title;
    // Restarted for each request, so superseding one does not inherit the
    // remains of the earlier request's timeout.
    m_pendingTimeoutTimer.start();
    if (m_playbackPending)
        return;
    m_playbackPending = true;
    emit playbackPendingChanged();
}

void RemoteControlController::playItemIds(
    const QStringList& itemIds, const QString& command, int startIndex, qint64 startPositionTicks)
{
    runPlay(itemIds, command, startIndex, startPositionTicks);
}

bool RemoteControlController::runPlay(
    QStringList itemIds, QString command, int startIndex, qint64 startPositionTicks, QString pendingTitle)
{
    if (!targetSelected() || !m_api)
        return false;
    itemIds.removeAll(QString());
    if (itemIds.isEmpty())
        return false;

    const bool startsPlayback = command == QStringLiteral("PlayNow") || command == QStringLiteral("PlayShuffle");
    const QString target = m_selectedSessionId;
    // Taking a generation retires the previous one, so a request still in
    // flight stops being able to change anything here. That is what lets a
    // newer choice supersede an older one: refusing it instead left the
    // target playing the old item, left this device showing the old item, and
    // discarded what the user actually asked for without saying so.
    const RequestGeneration::Token generation = m_playGeneration.next();
    if (startsPlayback) {
        const int requestedIndex = std::clamp(startIndex, 0, static_cast<int>(itemIds.size()) - 1);
        beginPendingPlayback(target, itemIds.at(requestedIndex), pendingTitle.trimmed());
        const QString subject = m_pendingTitle.isEmpty() ? QStringLiteral("playback") : m_pendingTitle;
        emit feedbackText(QStringLiteral("Starting %1 on %2…").arg(subject, m_selectedTargetName));
    }

    Async::runScoped(
        this, m_api->sendRemotePlay(target, std::move(itemIds), std::move(command), startPositionTicks, startIndex),
        [this, generation, startsPlayback]() {
            if (!m_playGeneration.isCurrent(generation))
                return;
            if (startsPlayback) {
                refreshTargets();
                m_pendingRefreshTimer.start();
            } else {
                QTimer::singleShot(300, this, &RemoteControlController::refreshTargets);
            }
        },
        [this, generation, startsPlayback](const std::exception_ptr& error) {
            if (!m_playGeneration.isCurrent(generation))
                return;
            if (startsPlayback) {
                const QString targetName = m_selectedTargetName;
                setPlaybackPending(false);
                refreshTargets();
                emit feedbackText(QStringLiteral("Could not start playback on %1").arg(targetName));
            }
            reportCommandError(QStringLiteral("start remote playback"), error);
        },
        "remote play command");
    return true;
}

void RemoteControlController::sendPlaystate(const QString& command, qint64 seekPositionTicks)
{
    if (!targetSelected() || !m_api)
        return;
    const QString target = m_selectedSessionId;
    const bool pauseCommand = command == QStringLiteral("Pause") || command == QStringLiteral("Unpause");
    const RequestGeneration::Token pauseGeneration
        = pauseCommand ? m_pauseGeneration.next() : m_pauseGeneration.current();
    Async::runScoped(
        this, m_api->sendRemotePlaystate(target, command, seekPositionTicks),
        [this]() { QTimer::singleShot(250, this, &RemoteControlController::refreshTargets); },
        [this, command, pauseCommand, pauseGeneration](const std::exception_ptr& error) {
            if (pauseCommand && m_pauseGeneration.isCurrent(pauseGeneration)) {
                m_pendingPaused.reset();
                m_pendingPauseDeadlineMs = 0;
                refreshTargets();
            }
            reportCommandError(command, error);
        },
        "remote playstate command");
}

void RemoteControlController::sendGeneralCommand(const QString& command, const QVariantMap& arguments)
{
    if (!targetSelected() || !m_api)
        return;
    const QString target = m_selectedSessionId;
    Async::runScoped(
        this, m_api->sendRemoteGeneralCommand(target, command, QJsonObject::fromVariantMap(arguments)),
        [this]() { QTimer::singleShot(250, this, &RemoteControlController::refreshTargets); },
        [this, command](const std::exception_ptr& error) { reportCommandError(command, error); },
        "remote general command");
}

void RemoteControlController::requestPaused(bool paused)
{
    if (!targetSelected())
        return;
    m_pendingPaused = paused;
    m_pendingPauseDeadlineMs = QDateTime::currentMSecsSinceEpoch() + 8'000;
    m_paused = paused;
    m_stateReceivedAtMs = QDateTime::currentMSecsSinceEpoch();
    updateMediaSession();
    emit stateChanged();
    sendPlaystate(paused ? QStringLiteral("Pause") : QStringLiteral("Unpause"));
}

void RemoteControlController::togglePause()
{
    requestPaused(!m_paused);
}

void RemoteControlController::stopPlayback()
{
    sendPlaystate(QStringLiteral("Stop"));
}

void RemoteControlController::nextTrack()
{
    sendPlaystate(QStringLiteral("NextTrack"));
}

void RemoteControlController::previousTrack()
{
    sendPlaystate(QStringLiteral("PreviousTrack"));
}

void RemoteControlController::seek(qint64 positionTicks)
{
    const qint64 clamped = std::clamp<qint64>(positionTicks, 0, m_runtimeTicks > 0 ? m_runtimeTicks : positionTicks);
    m_positionTicks = clamped;
    m_stateReceivedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_pendingSeekTicks = clamped;
    m_pendingSeekAtMs = m_stateReceivedAtMs;
    m_pendingSeekDeadlineMs = m_stateReceivedAtMs + kSeekConfirmTimeoutMs;
    sendPlaystate(QStringLiteral("Seek"), clamped);
    updateMediaSession();
    emit stateChanged();
}

void RemoteControlController::seekRelative(qint64 deltaTicks)
{
    seek(std::max<qint64>(0, predictedPositionTicks() + deltaTicks));
}

void RemoteControlController::setVolume(int volume)
{
    m_volume = std::clamp(volume, 0, 100);
    sendGeneralCommand(QStringLiteral("SetVolume"), { { QStringLiteral("Volume"), m_volume } });
    updateMediaSession();
    emit stateChanged();
}

void RemoteControlController::adjustVolume(int delta)
{
    setVolume(m_volume + delta);
}

void RemoteControlController::toggleMute()
{
    m_muted = !m_muted;
    sendGeneralCommand(m_muted ? QStringLiteral("Mute") : QStringLiteral("Unmute"));
    emit stateChanged();
}

void RemoteControlController::selectAudioTrack(int streamIndex)
{
    sendGeneralCommand(QStringLiteral("SetAudioStreamIndex"), { { QStringLiteral("Index"), streamIndex } });
}

void RemoteControlController::selectSubtitleTrack(int streamIndex)
{
    sendGeneralCommand(QStringLiteral("SetSubtitleStreamIndex"), { { QStringLiteral("Index"), streamIndex } });
}

void RemoteControlController::setRepeatMode(const QString& mode)
{
    m_repeatMode = mode;
    sendGeneralCommand(QStringLiteral("SetRepeatMode"), { { QStringLiteral("RepeatMode"), mode } });
    emit stateChanged();
}

void RemoteControlController::setShuffled(bool shuffled)
{
    m_shuffled = shuffled;
    sendGeneralCommand(QStringLiteral("SetShuffleQueue"),
        { { QStringLiteral("ShuffleMode"), shuffled ? QStringLiteral("Shuffle") : QStringLiteral("Sorted") } });
    emit stateChanged();
}

void RemoteControlController::playQueueItem(int index)
{
    if (index < 0 || index >= m_rawQueue.size())
        return;
    QStringList ids;
    for (const QJsonValue& value : m_rawQueue)
        ids.push_back(value.toObject().value(QStringLiteral("Id")).toString());
    runPlay(ids, QStringLiteral("PlayNow"), index, 0);
}

void RemoteControlController::moveQueueItem(int from, int to)
{
    if (from < 0 || from >= m_rawQueue.size() || to < 0 || to >= m_rawQueue.size() || from == to)
        return;
    QJsonArray reordered = m_rawQueue;
    const QJsonValue moved = reordered.takeAt(from);
    reordered.insert(to, moved);
    QStringList ids;
    int current = 0;
    const QString currentPlaylistId
        = m_sessions.value(m_selectedSessionId).value(QStringLiteral("PlaylistItemId")).toString();
    for (int index = 0; index < reordered.size(); ++index) {
        const QJsonObject row = reordered.at(index).toObject();
        ids.push_back(row.value(QStringLiteral("Id")).toString());
        if (!currentPlaylistId.isEmpty() && row.value(QStringLiteral("PlaylistItemId")).toString() == currentPlaylistId)
            current = index;
    }
    runPlay(ids, QStringLiteral("PlayNow"), current, predictedPositionTicks());
}

void RemoteControlController::removeQueueItem(int index)
{
    if (index < 0 || index >= m_rawQueue.size())
        return;
    QJsonArray remaining = m_rawQueue;
    const QJsonObject removed = remaining.takeAt(index).toObject();
    if (remaining.isEmpty()) {
        stopPlayback();
        return;
    }
    QStringList ids;
    int current = 0;
    const QString currentPlaylistId
        = m_sessions.value(m_selectedSessionId).value(QStringLiteral("PlaylistItemId")).toString();
    for (int rowIndex = 0; rowIndex < remaining.size(); ++rowIndex) {
        const QJsonObject row = remaining.at(rowIndex).toObject();
        ids.push_back(row.value(QStringLiteral("Id")).toString());
        if (!currentPlaylistId.isEmpty() && row.value(QStringLiteral("PlaylistItemId")).toString() == currentPlaylistId)
            current = rowIndex;
    }
    const bool removedCurrent = removed.value(QStringLiteral("PlaylistItemId")).toString() == currentPlaylistId;
    runPlay(ids, QStringLiteral("PlayNow"), current, removedCurrent ? 0 : predictedPositionTicks());
}

void RemoteControlController::updateMediaSession()
{
    if (!m_mediaSession || !targetSelected() || m_nowPlayingItem.isEmpty()) {
        if (m_mediaSession)
            m_mediaSession->clear();
        return;
    }
    RemoteMediaSessionState state;
    state.title = m_nowPlayingItem.value(QStringLiteral("title")).toString();
    state.artist = m_nowPlayingItem.value(QStringLiteral("seriesName")).toString();
    if (state.artist.isEmpty())
        state.artist = m_nowPlayingItem.value(QStringLiteral("albumArtist")).toString();
    state.album = m_nowPlayingItem.value(QStringLiteral("album")).toString();
    state.targetName = m_selectedTargetName;
    state.durationMs = m_runtimeTicks / 10'000;
    state.positionMs = predictedPositionTicks() / 10'000;
    state.playbackRate = m_playbackRate;
    state.volume = m_volume;
    state.playing = !m_paused;
    state.canSeek = m_runtimeTicks > 0;
    m_mediaSession->update(state);
}

void RemoteControlController::reportCommandError(const QString& action, const std::exception_ptr& error)
{
    emit errorText(QStringLiteral("Could not %1: %2").arg(action, exceptionMessage(error)));
}

bool RemoteControlController::handlePeerSignalling(const SpoolRemoteProtocol::Message& message)
{
    return m_link && m_link->handleServerMessage(message);
}

} // namespace JellyfinNative
