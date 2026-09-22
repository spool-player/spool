#include "GroupPlaybackController.h"

#include "../common/AsyncTask.h"
#include "../player/PlayQueueController.h"
#include "../player/PlayerController.h"
#include "../provider/SourceHub.h"

#include <QDateTime>
#include <QDebug>

#include <algorithm>
#include <cmath>
#include <limits>

namespace JellyfinNative {

namespace {
    constexpr int kGreedyTimeSyncIntervalMs = 1'000;
    constexpr int kSteadyTimeSyncIntervalMs = 60'000;
    // mpv retimes real audio output, so small clock errors stay inaudible and
    // a seek replaces rate correction once that would take too long.
    constexpr int kMinDelaySpeedToSyncMs = 100;
    constexpr int kMinDelaySkipToSyncMs = 400;
    constexpr int kSpeedToSyncDurationMs = 1'000;
    constexpr double kSpeedToSyncMinSpeed = 0.2;
    constexpr int kSyncMethodThresholdMs = 3'000;
    constexpr int kSyncCooldownMs = kSyncMethodThresholdMs / 2;
    constexpr double kSpeedToSyncMinRate = 0.97;
    constexpr double kSpeedToSyncMaxRate = 1.03;
    constexpr int kSpeedToSyncMaxDurationMs = 10'000;
    constexpr int kInternalSeekBufferingSuppressionMs = 3'000;
    constexpr qint64 kTicksPerSecond = 10'000'000;
    constexpr qint64 kTicksPerMillisecond = 10'000;

    qint64 ms(const QVariantMap& event, const char *key)
    {
        return event.value(QLatin1String(key)).toLongLong();
    }

    QString errorMessage(const QString& code)
    {
        static const QHash<QString, QString> messages {
            { QStringLiteral("group_missing"), QStringLiteral("That group no longer exists.") },
            { QStringLiteral("access_denied"), QStringLiteral("This account can't play what the group is watching.") },
            { QStringLiteral("create_denied"), QStringLiteral("The server didn't allow creating a group.") },
            { QStringLiteral("join_denied"), QStringLiteral("The server didn't allow joining that group.") },
            { QStringLiteral("disabled"), QStringLiteral("Watching together is turned off on this server.") },
        };
        return messages.value(code, QStringLiteral("Watch together: %1").arg(code));
    }
} // namespace

GroupCorrection GroupDriftPolicy::evaluate(double diffMs)
{
    const double absDiffMs = std::abs(diffMs);
    if (absDiffMs < kMinDelaySpeedToSyncMs)
        return {};
    if (absDiffMs < kMinDelaySkipToSyncMs) {
        // Keep the speed positive when the client is ahead by more than the
        // correction window allows.
        double speedToSyncTimeMs = kSpeedToSyncDurationMs;
        if (diffMs <= -speedToSyncTimeMs * kSpeedToSyncMinSpeed)
            speedToSyncTimeMs = absDiffMs / (1.0 - kSpeedToSyncMinSpeed);
        const double requestedSpeed = 1.0 + diffMs / speedToSyncTimeMs;
        const double speed = std::clamp(requestedSpeed, kSpeedToSyncMinRate, kSpeedToSyncMaxRate);
        if (speed != requestedSpeed) {
            // Recover the same distance at the bounded rate; hitting the cap
            // leaves a remainder for the next correction.
            speedToSyncTimeMs
                = std::min(absDiffMs / std::abs(speed - 1.0), static_cast<double>(kSpeedToSyncMaxDurationMs));
        }
        return { GroupCorrection::Method::Speed, speed, static_cast<int>(std::llround(speedToSyncTimeMs)) };
    }
    return { GroupCorrection::Method::Skip, 1.0, 0 };
}

GroupPlaybackController::GroupPlaybackController(
    SourceHub *hub, PlayerController *player, PlayQueueController *playQueue, QObject *parent)
    : GroupPlayback(parent)
    , m_hub(hub)
    , m_player(player)
    , m_playQueue(playQueue)
{
    m_timeSyncTimer.setSingleShot(true);
    m_commandTimer.setSingleShot(true);
    // mpv's seeking flag is often a short transition, not starvation; wait
    // a second before telling the group this client is buffering.
    m_bufferingDebounceTimer.setSingleShot(true);
    m_bufferingDebounceTimer.setInterval(1'000);
    m_speedCorrectionTimer.setSingleShot(true);
    m_correctionTimer.setInterval(1'000);
    connect(&m_timeSyncTimer, &QTimer::timeout, this, &GroupPlaybackController::requestTimeSync);
    connect(&m_commandTimer, &QTimer::timeout, this, &GroupPlaybackController::executeScheduledCommand);
    connect(&m_correctionTimer, &QTimer::timeout, this, &GroupPlaybackController::correctPlaybackDrift);
    connect(&m_speedCorrectionTimer, &QTimer::timeout, this, &GroupPlaybackController::finishSpeedCorrection);
    connect(&m_bufferingDebounceTimer, &QTimer::timeout, this, [this] { sendPlayerBufferingState(true); });
    connect(
        m_player, &PlayerController::playbackStateChanged, this, &GroupPlaybackController::handlePlayerStateChanged);
    connect(m_hub, &Provider::capabilitiesChanged, this, &GroupPlaybackController::availableChanged);
    connect(m_hub, &SourceHub::accountEvent, this,
        [this](const QString& account, const QString& type, const QVariantMap& payload) {
            if (type == QStringLiteral("group"))
                handleEvent(account, payload);
        });
}

bool GroupPlaybackController::available() const
{
    return m_hub->capabilities().testFlag(Provider::GroupPlayback);
}

qint64 GroupPlaybackController::serverNowMs() const
{
    return QDateTime::currentMSecsSinceEpoch() + static_cast<qint64>(std::llround(m_clock.offsetMs()));
}

void GroupPlaybackController::send(const QString& action, QVariantMap arguments, const char *failure)
{
    if (m_account.isEmpty())
        return;
    arguments.insert(QStringLiteral("action"), action);
    Async::runScoped(
        this, m_hub->call(m_account, QStringLiteral("groupSend"), arguments), [](QVariantMap) {},
        [this, failure](const std::exception_ptr& error) {
            if (failure)
                emit errorText(QStringLiteral("%1: %2").arg(QLatin1String(failure), exceptionMessage(error)));
        },
        "group request");
}

void GroupPlaybackController::refreshGroups()
{
    m_groups.clear();
    for (Provider *source : m_hub->sources()) {
        if (!source->capabilities().testFlag(Provider::GroupPlayback))
            continue;
        const QString account = source->id();
        const QString label = source->displayName();
        ++m_loadingGroups;
        Async::runScoped(
            this, m_hub->call(account, QStringLiteral("groups")),
            [this, account, label](QVariantMap result) {
                --m_loadingGroups;
                for (const QVariant& value : result.value(QStringLiteral("items")).toList()) {
                    QVariantMap group = value.toMap();
                    group.insert(
                        QStringLiteral("id"), m_hub->scoped(account, group.value(QStringLiteral("id")).toString()));
                    group.insert(QStringLiteral("accountId"), account);
                    group.insert(QStringLiteral("accountLabel"), label);
                    m_groups.append(group);
                }
                emit groupsChanged();
            },
            [this](const std::exception_ptr&) {
                --m_loadingGroups;
                emit groupsChanged();
            },
            "group list");
    }
    emit groupsChanged();
}

void GroupPlaybackController::createGroup(const QString& accountId, const QString& name)
{
    // The server announces the group over the account's socket before the
    // request returns, so events from this account count from now.
    m_account = accountId;
    Async::runScoped(
        this, m_hub->call(accountId, QStringLiteral("groupCreate"), { { QStringLiteral("name"), name } }),
        [this](QVariantMap) { beginTimeSync(); },
        [this](const std::exception_ptr& error) {
            if (m_groupId.isEmpty())
                m_account.clear();
            emit errorText(exceptionMessage(error));
        },
        "group create");
}

void GroupPlaybackController::joinGroup(const QString& groupId)
{
    const QString account = m_hub->accountOf(groupId);
    if (account.isEmpty())
        return;
    // As with creating: the joined event can arrive before the reply.
    m_account = account;
    Async::runScoped(
        this,
        m_hub->call(account, QStringLiteral("groupJoin"), { { QStringLiteral("groupId"), SourceHub::rawId(groupId) } }),
        [this, groupId](QVariantMap) {
            m_player->setSyncPlaybackSpeed(1.0);
            beginTimeSync();
            if (!m_groupId.isEmpty())
                return;
            m_groupId = SourceHub::rawId(groupId);
            for (const QVariant& group : std::as_const(m_groups)) {
                if (group.toMap().value(QStringLiteral("id")) == groupId)
                    applyInfo(group.toMap());
            }
            m_joinedAtServerMs = serverNowMs();
            emit groupChanged();
            sendPlayerBufferingState(true);
        },
        [this](const std::exception_ptr& error) {
            if (m_groupId.isEmpty())
                m_account.clear();
            emit errorText(exceptionMessage(error));
        },
        "group join");
}

void GroupPlaybackController::leaveGroup()
{
    if (m_groupId.isEmpty())
        return;
    const QString account = m_account;
    clearGroup();
    Async::runScoped(
        this, m_hub->call(account, QStringLiteral("groupLeave")), [](QVariantMap) { },
        [](const std::exception_ptr&) { }, "group leave");
}

void GroupPlaybackController::requestUnpauseWhenReady()
{
    if (!enabled())
        return;
    m_queueHandoff.arm();
    setWaitingForGroupPlayback(true);
}

void GroupPlaybackController::cancelPendingUnpause()
{
    m_queueHandoff.cancel();
    m_unpauseRequestPending = false;
    setWaitingForGroupPlayback(false);
}

void GroupPlaybackController::requestTogglePause()
{
    if (!enabled() || !m_player->sessionActive())
        return;
    if (m_player->paused()) {
        requestGroupUnpause();
    } else {
        // Pause here at once; the group's command then settles the position.
        m_player->setPaused(true);
        send(QStringLiteral("pause"), {}, "Pause");
    }
}

void GroupPlaybackController::requestSeek(double positionSeconds)
{
    if (!enabled() || !std::isfinite(positionSeconds))
        return;
    m_seekResume.arm(!m_player->paused());
    send(QStringLiteral("seek"),
        { { QStringLiteral("positionTicks"),
            QString::number(qint64(std::max(0.0, positionSeconds) * kTicksPerSecond)) } },
        "Seek");
}

void GroupPlaybackController::requestRelativeSeek(double deltaSeconds)
{
    requestSeek(m_player->positionSeconds() + deltaSeconds);
}

void GroupPlaybackController::requestNextItem()
{
    if (enabled() && !m_entryId.isEmpty())
        send(QStringLiteral("next"), { { QStringLiteral("entryId"), m_entryId } });
}

void GroupPlaybackController::requestPreviousItem()
{
    if (enabled() && !m_entryId.isEmpty())
        send(QStringLiteral("previous"), { { QStringLiteral("entryId"), m_entryId } });
}

void GroupPlaybackController::requestMoveItem(const QString& entryId, int newIndex)
{
    if (enabled() && !entryId.isEmpty())
        send(QStringLiteral("move"), { { QStringLiteral("entryId"), entryId }, { QStringLiteral("index"), newIndex } });
}

void GroupPlaybackController::requestRemoveItems(const QStringList& entryIds)
{
    if (enabled() && !entryIds.isEmpty())
        send(QStringLiteral("remove"), { { QStringLiteral("entryIds"), entryIds } });
}

void GroupPlaybackController::requestQueueItems(const QStringList& itemIds, bool queueNext)
{
    QStringList raw;
    for (const QString& id : itemIds) {
        if (m_hub->accountOf(id) == m_account)
            raw.append(SourceHub::rawId(id));
    }
    if (enabled() && !raw.isEmpty())
        send(QStringLiteral("queue"), { { QStringLiteral("itemIds"), raw }, { QStringLiteral("next"), queueNext } });
}

void GroupPlaybackController::requestPlayItem(const QString& entryId)
{
    if (enabled() && !entryId.isEmpty())
        send(QStringLiteral("play"), { { QStringLiteral("entryId"), entryId } });
}

QCoro::Task<void> GroupPlaybackController::publishQueue(
    QStringList itemIds, int playingIndex, qint64 startPositionTicks)
{
    QStringList raw;
    for (const QString& id : std::as_const(itemIds))
        raw.append(SourceHub::rawId(id));
    co_await m_hub->call(m_account, QStringLiteral("groupSend"),
        { { QStringLiteral("action"), QStringLiteral("setQueue") }, { QStringLiteral("itemIds"), raw },
            { QStringLiteral("index"), playingIndex },
            { QStringLiteral("positionTicks"), QString::number(startPositionTicks) } });
}

void GroupPlaybackController::handleEvent(const QString& accountId, const QVariantMap& event)
{
    const QString type = event.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("connected")) {
        if (accountId == m_account && enabled())
            beginTimeSync();
        return;
    }
    if (accountId != m_account || (m_groupId.isEmpty() && type != QStringLiteral("joined")))
        return;
    if (type == QStringLiteral("joined") || type == QStringLiteral("update")) {
        applyInfo(event);
        // The server stamps this and then, for a group already playing, sends
        // the command that brings us in line. Compare against the stamp, not a
        // local estimate taken a round trip later that would discard it.
        m_joinedAtServerMs = ms(event, "at") > 0 ? ms(event, "at") : serverNowMs();
        sendPlayerBufferingState(true);
    } else if (type == QStringLiteral("participants")) {
        m_participants = event.value(QStringLiteral("participants")).toStringList();
        emit groupChanged();
    } else if (type == QStringLiteral("participantJoined") || type == QStringLiteral("participantLeft")) {
        const QString name = event.value(QStringLiteral("name")).toString();
        m_participants.removeAll(name);
        if (type == QStringLiteral("participantJoined") && !name.isEmpty())
            m_participants.append(name);
        emit groupChanged();
    } else if (type == QStringLiteral("state")) {
        m_groupState = event.value(QStringLiteral("state")).toString();
        m_groupStateReason = event.value(QStringLiteral("reason")).toString();
        if (m_groupState == QStringLiteral("Waiting") && m_groupStateReason == QStringLiteral("Unpause"))
            setWaitingForGroupPlayback(true);
        else if (m_groupState == QStringLiteral("Paused")
            && (m_groupStateReason == QStringLiteral("Ready") || m_groupStateReason == QStringLiteral("Pause"))
            && !m_unpauseRequestPending)
            setWaitingForGroupPlayback(false);
        if (m_seekResume.takeWhenReady(m_groupState, m_groupStateReason))
            requestGroupUnpause();
        emit groupChanged();
    } else if (type == QStringLiteral("queue")) {
        applyQueue(event);
    } else if (type == QStringLiteral("command")) {
        handleCommand(event);
    } else if (type == QStringLiteral("left")) {
        clearGroup();
    } else if (type == QStringLiteral("error")) {
        emit errorText(errorMessage(event.value(QStringLiteral("code")).toString()));
    }
}

void GroupPlaybackController::applyInfo(const QVariantMap& info)
{
    const QString id = info.value(QStringLiteral("groupId")).toString();
    if (!id.isEmpty())
        m_groupId = id;
    m_groupName = info.value(QStringLiteral("name"), QStringLiteral("Watch together")).toString();
    m_groupState = info.value(QStringLiteral("state")).toString();
    m_groupStateReason = info.value(QStringLiteral("reason")).toString();
    m_participants = info.value(QStringLiteral("participants")).toStringList();
    if (!m_speedCorrectionActive)
        m_player->setSyncPlaybackSpeed(1.0);
    emit groupChanged();
}

void GroupPlaybackController::handleCommand(const QVariantMap& event)
{
    const qint64 emittedAt = ms(event, "emittedAt");
    if (emittedAt > 0 && m_joinedAtServerMs > 0 && emittedAt < m_joinedAtServerMs)
        return;
    const QString command = event.value(QStringLiteral("command")).toString();
    const qint64 serverTimeMs = ms(event, "at");
    const qint64 positionTicks = event.value(QStringLiteral("positionTicks")).toLongLong();
    const QString entryId = event.value(QStringLiteral("entryId")).toString();
    if (!entryId.isEmpty() && !m_entryId.isEmpty() && entryId != m_entryId)
        return;
    m_scheduledEntryId = entryId;
    const QString key = QStringLiteral("%1|%2|%3|%4").arg(command).arg(serverTimeMs).arg(positionTicks).arg(m_entryId);
    if (key == m_lastCommandKey && m_commandTimer.isActive())
        return;
    m_lastCommandKey = key;
    m_scheduledCommand = command;
    m_scheduledPositionTicks = positionTicks;
    m_scheduledServerTimeMs = serverTimeMs;
    m_commandDue = false;
    m_commandTimer.stop();
    m_bufferingDebounceTimer.stop();
    if (command == QStringLiteral("unpause"))
        setWaitingForGroupPlayback(true);
    const qint64 localNowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 delayMs = serverTimeMs <= 0 ? 0
        : m_clock.ready()                    ? m_clock.localDelayUntil(serverTimeMs, localNowMs)
                                             : std::max<qint64>(0, serverTimeMs - localNowMs);
    m_commandTimer.start(static_cast<int>(std::min<qint64>(delayMs, std::numeric_limits<int>::max())));
}

void GroupPlaybackController::applyQueue(const QVariantMap& event)
{
    const qint64 updatedAt = ms(event, "at");
    if (updatedAt > 0 && updatedAt < m_lastQueueUpdateMs)
        return;
    m_lastQueueUpdateMs = std::max(m_lastQueueUpdateMs, updatedAt);
    const QVariantList entries = event.value(QStringLiteral("items")).toList();
    const int playingIndex = event.value(QStringLiteral("index"), -1).toInt();
    if (entries.isEmpty() || playingIndex < 0 || playingIndex >= entries.size())
        return;
    QStringList itemIds;
    QStringList entryIds;
    for (const QVariant& value : entries) {
        const QVariantMap entry = value.toMap();
        itemIds.append(m_hub->scoped(m_account, entry.value(QStringLiteral("itemId")).toString()));
        entryIds.append(entry.value(QStringLiteral("entryId")).toString());
    }
    if (itemIds.contains(QString()))
        return;

    const quint64 generation = ++m_queueGeneration;
    m_queueHandoff.observeQueueUpdate();
    m_entryId = entryIds.at(playingIndex);
    const QString selectedItemId = itemIds.at(playingIndex);
    const qint64 requestedTicks = event.value(QStringLiteral("positionTicks")).toLongLong();

    // A long queue can take seconds to fill in; start the selected item
    // first, since reporting ready does not wait for the rest.
    const auto hydrate = [this, generation, itemIds, entryIds, playingIndex] {
        Async::runScoped(
            this, m_hub->fetchItemsByIds(itemIds),
            [this, generation, itemIds, entryIds, playingIndex](const std::vector<MovieItem>& fetched) {
                if (generation != m_queueGeneration)
                    return;
                std::vector<MovieItem> ordered;
                int resolvedIndex = -1;
                for (int i = 0; i < itemIds.size(); ++i) {
                    const auto found = std::find_if(fetched.begin(), fetched.end(),
                        [&](const MovieItem& item) { return item.id == itemIds.at(i); });
                    if (found == fetched.end())
                        continue;
                    MovieItem item = *found;
                    item.playlistItemId = entryIds.at(i);
                    if (i == playingIndex)
                        resolvedIndex = static_cast<int>(ordered.size());
                    ordered.push_back(std::move(item));
                }
                // Usually the echo of an edit made here: rebuilding would tear
                // down every row the queue panel is showing.
                if (m_playQueue->matchesQueue(ordered, resolvedIndex))
                    return;
                m_playQueue->setShuffled(false);
                if (resolvedIndex >= 0)
                    m_playQueue->playNow(ordered, resolvedIndex);
            },
            [](const std::exception_ptr&) {}, "group queue");
    };

    if (m_player->sessionActive() && m_playQueue->currentItem().id == selectedItemId) {
        m_queueLoading = false;
        hydrate();
        sendPlayerBufferingState(true);
        sendPendingUnpause();
        return;
    }
    m_queueLoading = true;
    setWaitingForGroupPlayback(true);
    Async::runScoped(
        this, m_hub->fetchItemsByIds({ selectedItemId }),
        [this, generation, selectedItemId, entry = m_entryId, requestedTicks, hydrate](
            const std::vector<MovieItem>& fetched) {
            if (generation != m_queueGeneration)
                return;
            m_queueLoading = false;
            const auto selected = std::find_if(
                fetched.cbegin(), fetched.cend(), [&](const MovieItem& item) { return item.id == selectedItemId; });
            MovieItem item = selected == fetched.cend() ? MovieItem {} : *selected;
            item.playlistItemId = entry;
            m_playQueue->setShuffled(false);
            if (item.id.isEmpty() || !m_playQueue->playNow(item)) {
                setWaitingForGroupPlayback(false);
                emit errorText(QStringLiteral("The group's current item isn't available here."));
                return;
            }
            m_waitingForPlaybackStart = true;
            m_playerStateKnown = false;
            send(QStringLiteral("buffering"),
                { { QStringLiteral("buffering"), true }, { QStringLiteral("playing"), false },
                    { QStringLiteral("positionTicks"), QString::number(std::max<qint64>(0, requestedTicks)) },
                    { QStringLiteral("entryId"), m_entryId }, { QStringLiteral("at"), serverNowMs() } });
            emit queuePlaybackRequested(requestedTicks);
            hydrate();
        },
        [this, generation](const std::exception_ptr& error) {
            if (generation != m_queueGeneration)
                return;
            m_queueLoading = false;
            setWaitingForGroupPlayback(false);
            emit errorText(exceptionMessage(error));
        },
        "group item");
}

void GroupPlaybackController::clearGroup()
{
    m_commandTimer.stop();
    m_speedCorrectionTimer.stop();
    m_bufferingDebounceTimer.stop();
    m_timeSyncTimer.stop();
    m_correctionTimer.stop();
    m_speedCorrectionActive = false;
    m_player->clearSyncPlaybackSpeed();
    m_groupId.clear();
    m_groupName.clear();
    m_groupState.clear();
    m_groupStateReason.clear();
    m_participants.clear();
    m_entryId.clear();
    m_lastCommandKey.clear();
    m_scheduledCommand.clear();
    m_scheduledEntryId.clear();
    m_scheduledPositionTicks = 0;
    m_scheduledServerTimeMs = 0;
    m_joinedAtServerMs = 0;
    m_queueHandoff.cancel();
    m_seekResume.cancel();
    ++m_queueGeneration;
    m_queueLoading = false;
    m_waitingForPlaybackStart = false;
    m_commandDue = false;
    m_unpauseRequestPending = false;
    m_syncCorrectionAttempts = 0;
    m_lastCorrectionAtMs = 0;
    m_suppressSeekBufferingUntilMs = 0;
    m_playerStateKnown = false;
    setWaitingForGroupPlayback(false);
    setPlaybackDiff(0, false);
    setSyncMethod(QStringLiteral("None"));
    emit groupChanged();
}

void GroupPlaybackController::executeScheduledCommand()
{
    if (m_scheduledCommand.isEmpty())
        return;
    if (m_scheduledCommand != QStringLiteral("stop")
        && (m_queueLoading || m_waitingForPlaybackStart || !m_player->sessionActive())) {
        m_commandDue = true;
        return;
    }
    if (!m_scheduledEntryId.isEmpty() && !m_entryId.isEmpty() && m_scheduledEntryId != m_entryId) {
        m_commandDue = false;
        return;
    }
    m_commandDue = false;
    const QString command = m_scheduledCommand;
    qint64 positionTicks = m_scheduledPositionTicks;
    if (command == QStringLiteral("unpause") && m_scheduledServerTimeMs > 0)
        positionTicks = m_clock.estimatePositionTicks(
            positionTicks, m_scheduledServerTimeMs, QDateTime::currentMSecsSinceEpoch());
    const double targetSeconds = static_cast<double>(positionTicks) / kTicksPerSecond;
    const double positionDelta = std::abs(m_player->positionSeconds() - targetSeconds);

    if (command == QStringLiteral("pause")) {
        m_seekResume.cancel();
        finishSpeedCorrection();
        m_unpauseRequestPending = false;
        setWaitingForGroupPlayback(false);
        m_player->setPaused(true);
        if (positionDelta > 0.1)
            m_player->seek(targetSeconds);
    } else if (command == QStringLiteral("unpause")) {
        finishSpeedCorrection();
        m_unpauseRequestPending = false;
        setWaitingForGroupPlayback(false);
        m_syncCorrectionAttempts = 0;
        // Re-enable correction half a threshold after unpausing, so drift
        // accumulated while the player spins up is not "corrected".
        m_lastCorrectionAtMs = QDateTime::currentMSecsSinceEpoch();
        setPlaybackDiff(0, false);
        setSyncMethod(QStringLiteral("None"));
        if (positionDelta * 1'000.0 > kMinDelaySkipToSyncMs && !m_player->seeking()) {
            m_suppressSeekBufferingUntilMs = QDateTime::currentMSecsSinceEpoch() + kInternalSeekBufferingSuppressionMs;
            m_player->seek(targetSeconds);
        }
        m_player->setPaused(false);
    } else if (command == QStringLiteral("seek")) {
        finishSpeedCorrection();
        m_player->setPaused(true);
        m_player->seek(targetSeconds);
        QTimer::singleShot(250, this, [this] { sendPlayerBufferingState(true); });
    } else if (command == QStringLiteral("stop")) {
        m_seekResume.cancel();
        finishSpeedCorrection();
        m_unpauseRequestPending = false;
        setWaitingForGroupPlayback(false);
        setPlaybackDiff(0, false);
        setSyncMethod(QStringLiteral("None"));
        m_player->stopWithReason(QStringLiteral("group-stop"));
    }
}

void GroupPlaybackController::correctPlaybackDrift()
{
    if (!enabled() || !m_player->sessionActive() || m_scheduledCommand != QStringLiteral("unpause")
        || m_scheduledServerTimeMs <= 0 || m_player->paused() || m_player->buffering() || m_player->seeking()) {
        setPlaybackDiff(0, false);
        if (m_speedCorrectionActive)
            finishSpeedCorrection();
        return;
    }
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 expectedTicks
        = m_clock.estimatePositionTicks(m_scheduledPositionTicks, m_scheduledServerTimeMs, nowMs);
    const qint64 actualTicks = static_cast<qint64>(m_player->estimatedPositionSeconds() * kTicksPerSecond);
    const qint64 signedDiffTicks = expectedTicks - actualTicks;
    setPlaybackDiff(signedDiffTicks, true);
    const GroupCorrection correction
        = GroupDriftPolicy::evaluate(static_cast<double>(signedDiffTicks) / kTicksPerMillisecond);
    if (correction.method == GroupCorrection::Method::None) {
        m_syncCorrectionAttempts = 0;
        if (!m_speedCorrectionActive)
            setSyncMethod(QStringLiteral("None"));
        return;
    }
    if (m_speedCorrectionActive) {
        // A large jump should not wait for gentle rate correction to finish.
        if (correction.method == GroupCorrection::Method::Speed)
            return;
        finishSpeedCorrection();
    }
    if (nowMs - m_lastCorrectionAtMs < kSyncCooldownMs)
        return;
    m_lastCorrectionAtMs = nowMs;
    ++m_syncCorrectionAttempts;
    if (correction.method == GroupCorrection::Method::Speed) {
        m_speedCorrectionActive = true;
        m_player->setSyncPlaybackSpeed(correction.speed);
        setSyncMethod(QStringLiteral("Speed ×%1").arg(correction.speed, 0, 'f', 2));
        m_speedCorrectionTimer.start(correction.durationMs);
        return;
    }
    setSyncMethod(QStringLiteral("Skip (%1)").arg(m_syncCorrectionAttempts));
    m_suppressSeekBufferingUntilMs = nowMs + kInternalSeekBufferingSuppressionMs;
    m_player->seek(static_cast<double>(expectedTicks) / kTicksPerSecond);
}

void GroupPlaybackController::finishSpeedCorrection()
{
    m_speedCorrectionTimer.stop();
    if (enabled())
        m_player->setSyncPlaybackSpeed(1.0);
    if (!m_speedCorrectionActive)
        return;
    m_speedCorrectionActive = false;
    setSyncMethod(QStringLiteral("None"));
}

void GroupPlaybackController::handlePlayerStateChanged()
{
    if (m_waitingForPlaybackStart) {
        // Buffering was already reported; say ready only once mpv has the file.
        if (m_player->sessionActive() && m_player->fileLoaded()) {
            m_waitingForPlaybackStart = false;
            if (m_commandDue)
                executeScheduledCommand();
            sendPlayerBufferingState(true);
            sendPendingUnpause();
        }
        return;
    }
    sendPlayerBufferingState(false);
    sendPendingUnpause();
}

void GroupPlaybackController::sendPlayerBufferingState(bool force)
{
    if (!enabled() || !m_player->sessionActive())
        return;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool internalSeek = m_player->seeking() && nowMs < m_suppressSeekBufferingUntilMs;
    const bool buffering = !internalSeek && (m_player->buffering() || m_player->seeking());
    if (!force && buffering && (!m_playerStateKnown || !m_lastPlayerBuffering)) {
        if (!m_bufferingDebounceTimer.isActive())
            m_bufferingDebounceTimer.start();
        return;
    }
    if (!buffering)
        m_bufferingDebounceTimer.stop();
    if (!force && m_playerStateKnown && buffering == m_lastPlayerBuffering)
        return;
    m_playerStateKnown = true;
    m_lastPlayerBuffering = buffering;
    send(QStringLiteral("buffering"),
        { { QStringLiteral("buffering"), buffering }, { QStringLiteral("playing"), !m_player->paused() },
            { QStringLiteral("positionTicks"), QString::number(qint64(m_player->positionSeconds() * kTicksPerSecond)) },
            { QStringLiteral("entryId"), m_entryId }, { QStringLiteral("at"), serverNowMs() } });
}

void GroupPlaybackController::sendPendingUnpause()
{
    if (!enabled()
        || !m_queueHandoff.canSend(
            m_queueLoading, m_waitingForPlaybackStart, m_player->sessionActive(), m_player->fileLoaded()))
        return;
    m_queueHandoff.cancel();
    requestGroupUnpause();
}

void GroupPlaybackController::requestGroupUnpause()
{
    if (!enabled() || m_unpauseRequestPending)
        return;
    m_unpauseRequestPending = true;
    setWaitingForGroupPlayback(true);
    Async::runScoped(
        this,
        m_hub->call(
            m_account, QStringLiteral("groupSend"), { { QStringLiteral("action"), QStringLiteral("unpause") } }),
        [](QVariantMap) {},
        [this](const std::exception_ptr& error) {
            m_unpauseRequestPending = false;
            setWaitingForGroupPlayback(false);
            emit errorText(exceptionMessage(error));
        },
        "group unpause");
}

void GroupPlaybackController::setWaitingForGroupPlayback(bool waiting)
{
    if (m_waitingForGroupPlayback != waiting) {
        m_waitingForGroupPlayback = waiting;
        emit syncStatusChanged();
    }
}

void GroupPlaybackController::setPlaybackDiff(qint64 diffTicks, bool valid)
{
    const double diffMs = valid ? static_cast<double>(diffTicks) / kTicksPerMillisecond : 0.0;
    if (m_playbackDiffValid == valid && (!valid || qFuzzyCompare(m_playbackDiffMs, diffMs)))
        return;
    m_playbackDiffValid = valid;
    m_playbackDiffMs = diffMs;
    emit syncStatusChanged();
}

void GroupPlaybackController::setSyncMethod(const QString& method)
{
    if (m_syncMethod != method) {
        m_syncMethod = method;
        emit syncStatusChanged();
    }
}

void GroupPlaybackController::beginTimeSync()
{
    m_timeSyncTimer.stop();
    m_clock.reset();
    m_greedyTimeSyncRemaining = 3;
    m_correctionTimer.start();
    requestTimeSync();
}

void GroupPlaybackController::requestTimeSync()
{
    if (m_account.isEmpty() || m_timeSyncInFlight)
        return;
    m_timeSyncInFlight = true;
    const qint64 sentMs = QDateTime::currentMSecsSinceEpoch();
    Async::runScoped(
        this, m_hub->call(m_account, QStringLiteral("clock")),
        [this, sentMs](QVariantMap response) {
            m_timeSyncInFlight = false;
            const qint64 receivedMs = QDateTime::currentMSecsSinceEpoch();
            const qint64 serverReceived = ms(response, "received");
            const qint64 serverSent = ms(response, "sent");
            if (serverReceived > 0 && serverSent > 0) {
                m_clock.addMeasurement({ sentMs, serverReceived, serverSent, receivedMs });
                emit clockChanged();
                send(QStringLiteral("ping"), { { QStringLiteral("ms"), qint64(std::llround(m_clock.pingMs())) } });
            }
            const int interval = m_greedyTimeSyncRemaining > 0 ? kGreedyTimeSyncIntervalMs : kSteadyTimeSyncIntervalMs;
            m_greedyTimeSyncRemaining = std::max(0, m_greedyTimeSyncRemaining - 1);
            if (enabled())
                m_timeSyncTimer.start(interval);
        },
        [this](const std::exception_ptr&) {
            m_timeSyncInFlight = false;
            if (enabled())
                m_timeSyncTimer.start(kSteadyTimeSyncIntervalMs);
        },
        "group clock");
}

} // namespace JellyfinNative
