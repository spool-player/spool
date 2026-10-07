#include "RemoteTargetsController.h"

#include "../common/AsyncTask.h"
#include "../platform/PlatformRemoteMediaSession.h"
#include "../provider/ProviderRegistry.h"
#include "../provider/ProviderUiContext.h"
#include "../provider/SourceHub.h"
#include "GroupPlaybackController.h"
#include "TrickplayService.h"

#include <QGuiApplication>
#include <QPointer>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Spool {
namespace {
    const QString ListScope = QStringLiteral("remote-target-list");
    const QString ConnectScope = QStringLiteral("remote-target-connect");
    const QString ReadScope = QStringLiteral("remote-target-state");
    const QString CommandScope = QStringLiteral("remote-target-command");
    const QString QueueScope = QStringLiteral("remote-target-queue");
    QVariantMap action(const char *name)
    {
        return { { "action", QLatin1String(name) } };
    }
    bool gone(const QString& code)
    {
        return code.contains("target_unavailable") || code.contains("target_not_found")
            || code.contains("remote_target_gone") || code.contains("target_unauthorized")
            || code.contains("source_unavailable") || code.contains("unsupported_capability");
    }
}

RemoteTargetsController::RemoteTargetsController(
    SourceHub *hub, ProviderRegistry *registry, GroupPlaybackController *group, QObject *parent)
    : QObject(parent)
    , m_hub(hub)
    , m_registry(registry)
    , m_group(group)
    , m_mediaSession(createPlatformRemoteMediaSession())
{
    m_clock.start();
    rebuildTargets();
    connect(&m_pollTimer, &QTimer::timeout, this, &RemoteTargetsController::requestState);
    m_listTimer.setInterval(30000);
    connect(&m_listTimer, &QTimer::timeout, this, &RemoteTargetsController::refreshTargets);
    m_queueTimer.setInterval(5000);
    // Coarse timers may fire early: the minimum-age guard would then skip an
    // entire interval and turn a five-second refresh into a ten-second wait.
    m_queueTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_queueTimer, &QTimer::timeout, this, [this] {
        if (m_state.value("queueRevision").toString().isEmpty() && m_clock.elapsed() - m_lastQueueRead >= 5000)
            requestQueuePage(true);
    });
    if (auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        m_foreground = app->applicationState() == Qt::ApplicationActive;
        connect(app, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) { setForeground(state == Qt::ApplicationActive); });
    }
    connect(hub, &SourceHub::capabilitySupportChanged, this, [this](const QString& account) {
        emit availableChanged();
        if (!m_hub->remoteAvailable(account)) {
            m_accountTargets.remove(account);
            rebuildTargets();
            if (m_selection.accountId == account)
                failTarget(
                    m_selection, tr("The selected target is no longer available. Choose a target or This device."));
        } else if (m_selection.accountId == account) {
            requestState();
        }
        if (m_chooserVisible && m_foreground)
            refreshTargets();
    });
    connect(registry, &ProviderRegistry::accountsChanged, this, [this] {
        emit availableChanged();
        if (!m_selection.targetId.isEmpty() && !m_hub->remoteAvailable(m_selection.accountId))
            failTarget(
                m_selection, tr("Remote control is unavailable for this account. Choose This device to play locally."));
    });
    connect(registry, &ProviderRegistry::sourceStarted, this, [this](Provider *) {
        emit availableChanged();
        if (m_chooserVisible && m_foreground)
            refreshTargets();
    });
    connect(registry, &ProviderRegistry::sourceStopped, this, [this](const QString& account) {
        if (m_trickplay && m_selection.accountId == account) {
            m_trickplay->clear();
            m_state.remove("preview");
            emit stateChanged();
        }
    });
    connect(hub, &SourceHub::accountEvent, this,
        [this](const QString& account, const QString& type, const QVariantMap& payload) {
            if (type != "remoteChanged" || account != m_selection.accountId || !m_foreground
                || payload.value("targetId").toString() != m_selection.targetId)
                return;
            // An event invalidates only this account/target. Never translate it into
            // an inbound command and never rediscover all accounts in response.
            requestState();
        });
    if (group) {
        connect(group, &GroupPlaybackController::groupChanged, this, [this] {
            if (m_group->enabled()) {
                if (!m_selection.targetId.isEmpty())
                    disconnectTarget(); // detach only: the peer keeps playing
            } else if (m_leavingGroup && !m_pendingTarget.isEmpty()) {
                const QString target = m_pendingTarget;
                m_pendingTarget.clear();
                m_leavingGroup = false;
                emit groupLeaveConfirmationChanged();
                selectTarget(target);
            }
        });
    }
    connect(
        m_mediaSession.get(), &PlatformRemoteMediaSession::playRequested, this, [this] { send(action("unpause")); });
    connect(m_mediaSession.get(), &PlatformRemoteMediaSession::pauseRequested, this, [this] { send(action("pause")); });
    connect(m_mediaSession.get(), &PlatformRemoteMediaSession::playPauseRequested, this,
        [this] { send(action(m_state.value("state") == "playing" ? "pause" : "unpause")); });
    connect(m_mediaSession.get(), &PlatformRemoteMediaSession::stopRequested, this, [this] { send(action("stop")); });
    connect(m_mediaSession.get(), &PlatformRemoteMediaSession::nextRequested, this, [this] { send(action("next")); });
    connect(m_mediaSession.get(), &PlatformRemoteMediaSession::previousRequested, this,
        [this] { send(action("previous")); });
    connect(m_mediaSession.get(), &PlatformRemoteMediaSession::seekRequested, this, [this](qint64 ms) {
        if (ms >= 0 && ms <= std::numeric_limits<qint64>::max() / 10000)
            send({ { "action", "seek" }, { "positionTicks", QString::number(ms * 10000) } });
    });
    connect(m_mediaSession.get(), &PlatformRemoteMediaSession::seekRelativeRequested, this, [this](qint64 delta) {
        if (m_position.isEmpty())
            return;
        const qint64 maximum = m_runtime.isEmpty() ? std::numeric_limits<qint64>::max() : m_runtime.toLongLong();
        qint64 ticks = std::clamp(m_position.toLongLong(), qint64(0), maximum);
        if (delta > 0)
            ticks = delta > (maximum - ticks) / 10000 ? maximum : ticks + delta * 10000;
        else if (delta < 0)
            ticks = delta < -(ticks / 10000) ? 0 : ticks + delta * 10000;
        send({ { "action", "seek" }, { "positionTicks", QString::number(ticks) } });
    });
    connect(m_mediaSession.get(), &PlatformRemoteMediaSession::volumeRequested, this,
        [this](int volume) { send({ { "action", "volume" }, { "value", volume } }); });
}

RemoteTargetsController::~RemoteTargetsController()
{
    cancelSelection();
    for (const auto& account : std::as_const(m_listPending))
        m_registry->cancelSourceScope(account, ListScope);
    m_mediaSession->clear();
}

QVariantMap RemoteTargetsController::state() const
{
    auto result = m_state;
    if (!m_position.isEmpty())
        result.insert("positionTicks", m_position);
    if (!m_runtime.isEmpty())
        result.insert("runtimeTicks", m_runtime);
    return result;
}

bool RemoteTargetsController::isCurrent(const Selection& selection) const
{
    return selection.generation == m_selection.generation && selection.targetId == m_selection.targetId
        && selection.accountId == m_selection.accountId;
}

bool RemoteTargetsController::available() const
{
    return !m_selection.targetId.isEmpty() || !m_problem.isEmpty()
        || std::any_of(m_registry->accountList().begin(), m_registry->accountList().end(),
            [this](const ProviderAccount& account) { return m_hub->remoteAvailable(account.id); });
}

void RemoteTargetsController::setProblem(QString value)
{
    if (m_problem != value) {
        m_problem = std::move(value);
        emit problemChanged();
        emit availableChanged();
    }
}

void RemoteTargetsController::updateTimers()
{
    const bool attached = m_foreground && !m_selection.targetId.isEmpty() && !m_connecting;
    const int interval = m_controlsVisible ? 1000 : 3000;
    if (attached) {
        if (!m_pollTimer.isActive() || m_pollTimer.interval() != interval)
            m_pollTimer.start(interval);
    } else {
        m_pollTimer.stop();
    }
    if (m_foreground && m_chooserVisible) {
        if (!m_listTimer.isActive())
            m_listTimer.start();
    } else {
        m_listTimer.stop();
    }
    if (attached && m_queueVisible) {
        if (!m_queueTimer.isActive())
            m_queueTimer.start();
    } else {
        m_queueTimer.stop();
    }
}

void RemoteTargetsController::setControlsVisible(bool value)
{
    if (m_controlsVisible == value)
        return;
    m_controlsVisible = value;
    updateTimers();
}
void RemoteTargetsController::setChooserVisible(bool value)
{
    if (m_chooserVisible == value)
        return;
    m_chooserVisible = value;
    if (value && m_foreground)
        refreshTargets();
    else {
        ++m_listGeneration;
        for (const auto& account : std::as_const(m_listPending))
            m_registry->cancelSourceScope(account, ListScope);
        m_listPending.clear();
        m_listActive = 0;
        emit busyChanged();
    }
    updateTimers();
    emit visibilityChanged();
}
void RemoteTargetsController::setQueueVisible(bool value)
{
    if (m_queueVisible == value)
        return;
    m_queueVisible = value;
    if (value)
        requestQueuePage(true);
    else {
        ++m_queueGeneration;
        m_registry->cancelSourceScope(m_selection.accountId, QueueScope);
        m_queueBusy = false;
        m_queueResetPending = false;
        emit queueChanged();
    }
    updateTimers();
    emit visibilityChanged();
}
void RemoteTargetsController::setForeground(bool value)
{
    if (m_foreground == value)
        return;
    m_foreground = value;
    if (value) {
        requestState();
        if (m_chooserVisible)
            refreshTargets();
        if (m_queueVisible)
            requestQueuePage(true);
    } else {
        ++m_commandEpoch;
        ++m_readSerial;
        ++m_queueGeneration;
        ++m_listGeneration;
        m_registry->cancelSourceScope(m_selection.accountId, ReadScope);
        m_registry->cancelSourceScope(m_selection.accountId, QueueScope);
        for (const auto& account : std::as_const(m_listPending))
            m_registry->cancelSourceScope(account, ListScope);
        m_listPending.clear();
        m_listActive = 0;
        m_readBusy = m_readAgain = m_queueBusy = m_queueResetPending = false;
        emit busyChanged();
        emit queueChanged();
    }
    updateTimers();
}

void RemoteTargetsController::refreshTargets()
{
    if (!m_foreground || !m_chooserVisible || m_listActive > 0)
        return;
    ++m_listGeneration;
    m_listNext = 0;
    m_listAccounts.clear();
    for (const auto& account : m_registry->accountList())
        if (m_hub->remoteAvailable(account.id))
            m_listAccounts.append(account.id);
    for (auto it = m_accountTargets.begin(); it != m_accountTargets.end();) {
        if (!m_listAccounts.contains(it.key()))
            it = m_accountTargets.erase(it);
        else
            ++it;
    }
    rebuildTargets();
    launchListRequests();
}
void RemoteTargetsController::launchListRequests()
{
    while (m_listActive < 4 && m_listNext < m_listAccounts.size()) {
        const auto account = m_listAccounts.at(m_listNext);
        if (m_listPending.contains(account))
            break;
        ++m_listNext;
        const auto generation = m_listGeneration;
        ++m_listActive;
        m_listPending.insert(account);
        Async::runScoped(
            this, m_hub->remoteTargets(account, ListScope),
            [this, account, generation](
                QVariantList targets) { finishList(account, generation, std::move(targets), false); },
            [this, account, generation](const std::exception_ptr&) { finishList(account, generation, {}, true); },
            "remote target discovery");
    }
    emit busyChanged();
}
void RemoteTargetsController::finishList(QString account, quint64 generation, QVariantList targets, bool failed)
{
    if (generation != m_listGeneration)
        return;
    --m_listActive;
    m_listPending.remove(account);
    if (!failed) {
        m_accountTargets.insert(account, std::move(targets));
        if (m_selection.accountId == account) {
            const auto& rows = m_accountTargets[account];
            const auto found = std::find_if(rows.begin(), rows.end(),
                [this](const QVariant& row) { return row.toMap().value("id") == m_selection.targetId; });
            if (found == rows.end())
                failTarget(m_selection, tr("The selected target disappeared. Choose a target or This device."));
            else {
                m_target = found->toMap();
                emit selectionChanged();
            }
        }
    } else {
        m_accountTargets.remove(account);
        setProblem(tr("Some remote targets could not be discovered. Refresh to try again."));
    }
    rebuildTargets();
    launchListRequests();
}
void RemoteTargetsController::rebuildTargets()
{
    QVariantList rows { QVariantMap { { "id", QString() }, { "name", tr("This device") }, { "isLocal", true },
        { "commands", QStringList() }, { "queueEditing", "none" } } };
    // Persistent connection order, with progressive results within that order.
    for (const auto& account : m_registry->accountList()) {
        for (const auto& value : m_accountTargets.value(account.id)) {
            auto row = value.toMap();
            row.insert("accountLabel", account.label);
            if (const auto *module = m_registry->module(account.module))
                row.insert("providerName", module->manifest.name);
            QString detail = row.value("providerName").toString() + QStringLiteral(" · ") + account.label;
            if (!row.value("detail").toString().isEmpty())
                detail += QStringLiteral(" — ") + row.value("detail").toString();
            row.insert("detail", detail);
            rows.append(row);
        }
    }
    if (rows != m_targets) {
        m_targets = std::move(rows);
        emit targetsChanged();
    }
}

void RemoteTargetsController::selectTarget(QString targetId)
{
    if (targetId.isEmpty()) {
        disconnectTarget();
        return;
    }
    if (targetId == m_selection.targetId)
        return;
    auto found = std::find_if(
        m_targets.begin(), m_targets.end(), [&](const QVariant& row) { return row.toMap().value("id") == targetId; });
    if (found == m_targets.end()) {
        setProblem(tr("That target is no longer available. Refresh the target list."));
        return;
    }
    if (m_group && m_group->enabled()) {
        m_pendingTarget = targetId;
        m_leavingGroup = false;
        emit groupLeaveConfirmationChanged();
        return;
    }
    connectTarget(found->toMap());
}
void RemoteTargetsController::confirmLeaveGroup(bool accepted)
{
    if (m_pendingTarget.isEmpty())
        return;
    if (!accepted) {
        m_pendingTarget.clear();
        m_leavingGroup = false;
        emit groupLeaveConfirmationChanged();
        return;
    }
    if (m_group && m_group->enabled()) {
        m_leavingGroup = true;
        m_group->leaveGroup();
    } else {
        const QString target = m_pendingTarget;
        m_pendingTarget.clear();
        emit groupLeaveConfirmationChanged();
        selectTarget(target);
    }
}
void RemoteTargetsController::cancelSelection()
{
    if (m_trickplay)
        m_trickplay->clear();
    const auto account = m_selection.accountId;
    ++m_selection.generation;
    ++m_commandEpoch;
    ++m_readSerial;
    ++m_queueGeneration;
    if (m_advancedControls) {
        m_advancedControls->close();
        m_advancedControls.clear();
    }
    for (const auto& scope : { ConnectScope, ReadScope, CommandScope, QueueScope })
        if (!account.isEmpty())
            m_registry->cancelSourceScope(account, scope);
}
void RemoteTargetsController::clearQueue()
{
    m_queue.clear();
    m_cursor.reset();
    m_seenCursors.clear();
    m_queuePages = 0;
    m_queueExhausted = true;
    m_queueBusy = m_queueResetPending = false;
    m_loadedRevision.clear();
    emit queueChanged();
}
void RemoteTargetsController::disconnectTarget()
{
    cancelSelection();
    m_selection.targetId.clear();
    m_selection.accountId.clear();
    m_target.clear();
    m_state.clear();
    m_position.clear();
    m_runtime.clear();
    m_optimistic.clear();
    m_ackSequence.reset();
    m_connecting = m_commandBusy = m_readBusy = m_readAgain = false;
    m_pendingTarget.clear();
    m_leavingGroup = false;
    clearQueue();
    m_mediaSession->clear();
    updateTimers();
    setProblem({});
    emit selectionChanged();
    emit stateChanged();
    emit positionChanged();
    emit busyChanged();
    emit groupLeaveConfirmationChanged();
}
void RemoteTargetsController::failTarget(const Selection& selection, const QString& problem)
{
    if (!isCurrent(selection))
        return;
    disconnectTarget();
    setProblem(problem);
}
void RemoteTargetsController::connectTarget(QVariantMap target)
{
    disconnectTarget();
    m_target = std::move(target);
    m_selection.targetId = m_target.value("id").toString();
    m_selection.accountId = m_target.value("accountId").toString();
    m_connecting = true;
    const auto selected = m_selection;
    emit selectionChanged();
    emit busyChanged();
    Async::runScoped(
        this, connectSelected(selected, m_target), [] {},
        [this, selected](const std::exception_ptr&) {
            failTarget(selected,
                tr("Could not connect to this target. No playback was transferred. Choose a target or This device."));
        },
        "remote target connection");
}
QCoro::Task<void> RemoteTargetsController::connectSelected(Selection selected, QVariantMap target)
{
    QPointer<RemoteTargetsController> guard(this);
    const auto origins = target.value("origins").toStringList();
    if (!origins.isEmpty()) {
        // Only the preferred endpoint is granted, never all discovered peers.
        // The provider chooses its endpoint consistently with this first entry.
        co_await m_registry->requestAccountOrigin(selected.accountId, QUrl(origins.first()), ConnectScope);
        if (!guard || !isCurrent(selected))
            co_return;
    }
    auto response = co_await m_hub->remoteState(selected.targetId, true, ConnectScope);
    if (!guard || !isCurrent(selected))
        co_return;
    m_connecting = false;
    applyState(std::move(response));
    updateTimers();
    emit busyChanged();
    if (m_queueVisible)
        requestQueuePage(true);
    if (m_foreground && m_chooserVisible) {
        // Selection can verify richer peer capabilities after origin consent.
        // Refresh just this account through the same four-slot chooser scheduler.
        if (m_listActive == 0) {
            m_listAccounts.clear();
            m_listNext = 0;
        }
        m_listAccounts.append(selected.accountId);
        launchListRequests();
    }
}

void RemoteTargetsController::requestState()
{
    if (!m_foreground || m_selection.targetId.isEmpty() || m_connecting)
        return;
    if (m_readBusy || m_commandBusy) {
        m_readAgain = true;
        return;
    }
    m_readBusy = true;
    m_readAgain = false;
    const auto selected = m_selection;
    const auto epoch = m_commandEpoch;
    const auto serial = ++m_readSerial;
    Async::runScoped(
        this, m_hub->remoteState(selected.targetId, false, ReadScope),
        [this, selected, epoch, serial](QVariantMap response) {
            if (!isCurrent(selected) || serial != m_readSerial)
                return;
            m_readBusy = false;
            if (epoch == m_commandEpoch && m_foreground)
                applyState(std::move(response));
            if (m_readAgain)
                requestState();
        },
        [this, selected, epoch, serial](const std::exception_ptr& error) {
            if (!isCurrent(selected) || serial != m_readSerial)
                return;
            m_readBusy = false;
            if (epoch == m_commandEpoch && m_foreground)
                failTarget(selected,
                    gone(exceptionMessage(error))
                        ? tr("The selected target is unavailable. Choose a target or This device.")
                        : tr("The remote target could not be reached. Playback was not moved to this device."));
            else if (m_readAgain)
                requestState();
        },
        "remote target state");
}
void RemoteTargetsController::applyState(QVariantMap response)
{
    const auto now = m_clock.elapsed();
    const bool acknowledged = m_ackSequence && response.contains("commandSequence")
        && response.value("commandSequence").toULongLong() >= *m_ackSequence;
    if (!m_optimistic.isEmpty() && now < m_optimisticUntil && !acknowledged) {
        for (auto it = m_optimistic.begin(); it != m_optimistic.end();) {
            bool confirmed = response.value(it.key()) == it.value();
            if (it.key() == "positionTicks" && response.contains(it.key())) {
                const long double advance = m_state.value("state") == "playing"
                    ? static_cast<long double>(now - m_optimisticAt) * 10000 * m_state.value("rate", 1.0).toDouble()
                    : 0;
                const long double expected = it.value().toString().toLongLong() + advance;
                confirmed
                    = std::abs(static_cast<long double>(response.value(it.key()).toString().toLongLong()) - expected)
                    <= 25000000;
            }
            if (confirmed && !m_ackSequence)
                it = m_optimistic.erase(it);
            else {
                response.insert(it.key(), it.value());
                ++it;
            }
        }
    } else {
        m_optimistic.clear();
        m_ackSequence.reset();
    }
    const QString position = response.take("positionTicks").toString();
    const QString runtime = response.take("runtimeTicks").toString();
    const bool positionChangedValue = position != m_position || runtime != m_runtime;
    m_position = position;
    m_runtime = runtime;
    // A genuine acknowledgement is transport state, not a media model change.
    response.remove("commandSequence");
    const QString revision = response.value("queueRevision").toString();
    const bool revisionChanged = revision != m_state.value("queueRevision").toString();
    if (m_trickplay
        && (m_state.value("preview") != response.value("preview") || m_state.value("item") != response.value("item"))) {
        const auto descriptor = response.value("preview").toMap();
        TrickplayInfo info;
        info.width = descriptor.value("width").toInt();
        info.height = descriptor.value("height").toInt();
        info.tileWidth = descriptor.value("columns").toInt();
        info.tileHeight = descriptor.value("rows").toInt();
        info.thumbnailCount = descriptor.value("count").toInt();
        info.intervalMs = descriptor.value("intervalMs").toInt();
        info.urlTemplate = descriptor.value("urlTemplate").toString();
        info.format = descriptor.value("format").toString();
        info.url = descriptor.value("url").toString();
        m_trickplay->setSession(info, position.toDouble() / 10000000.0);
    }
    if (m_state != response) {
        m_state = std::move(response);
        emit stateChanged();
    }
    if (positionChangedValue)
        emit positionChanged();
    updateMediaSession();
    if (m_queueVisible && !revision.isEmpty() && (revisionChanged || (!m_queueBusy && revision != m_loadedRevision)))
        requestQueuePage(true);
}

void RemoteTargetsController::beginOptimistic(const QVariantMap& command)
{
    const auto name = command.value("action").toString();
    m_ackSequence.reset();
    if (name == "pause" || name == "unpause" || name == "stop")
        m_optimistic.insert("state", name == "pause" ? "paused" : name == "unpause" ? "playing" : "stopped");
    else if (name == "seek")
        m_optimistic.insert("positionTicks", command.value("positionTicks"));
    else if (name == "volume")
        m_optimistic.insert("volume", command.value("value"));
    else if (name == "mute")
        m_optimistic.insert("muted", command.value("value"));
    else if (name == "shuffle")
        m_optimistic.insert("shuffled", command.value("value"));
    else if (name == "repeat")
        m_optimistic.insert("repeatMode", command.value("mode"));
    if (name == "seek")
        m_optimisticAt = m_clock.elapsed();
    m_optimisticUntil = m_clock.elapsed() + 8000;
    auto snapshot = state();
    for (auto it = m_optimistic.cbegin(); it != m_optimistic.cend(); ++it)
        snapshot.insert(it.key(), it.value());
    // Publish directly; applyState would treat our own optimistic snapshot as
    // confirmation from the backend.
    const auto position = snapshot.take("positionTicks").toString();
    snapshot.remove("runtimeTicks");
    if (snapshot != m_state) {
        m_state = std::move(snapshot);
        emit stateChanged();
    }
    if (position != m_position) {
        m_position = position;
        emit positionChanged();
    }
    updateMediaSession();
}

QCoro::Task<bool> RemoteTargetsController::command(Selection selected, QVariantMap value)
{
    if (!isCurrent(selected) || selected.targetId.isEmpty() || m_connecting || m_commandBusy)
        co_return false;
    const auto name = value.value("action").toString();
    if (!m_state.value("commands").toStringList().contains(name)) {
        setProblem(tr("This target does not support that command."));
        co_return false;
    }
    if (name == "queuePlay" || name == "queueRemove" || name == "queueMove") {
        const auto entry = value.value("entryId").toString();
        auto found = std::find_if(
            m_queue.begin(), m_queue.end(), [&](const QVariant& row) { return row.toMap().value("entryId") == entry; });
        if (m_queueBusy || found == m_queue.end()
            || ((name == "queueRemove" || name == "queueMove") && m_target.value("queueEditing") == "none")) {
            setProblem(tr("Refresh the queue before changing that entry."));
            co_return false;
        }
        if (name == "queueMove") {
            bool validIndex = false;
            const int index = value.value("index").toInt(&validIndex);
            const int from = int(std::distance(m_queue.begin(), found));
            if (!validIndex || index < 0 || index >= m_queue.size() || value.value("index").toDouble() != index) {
                setProblem(tr("Load the adjacent queue page before moving across its boundary."));
                co_return false;
            }
            const int predecessor = index == 0 ? -1 : index > from ? index : index - 1;
            const QVariant after
                = predecessor < 0 ? QVariant::fromValue(nullptr) : m_queue.at(predecessor).toMap().value("entryId");
            if (!value.contains("afterEntryId")
                || (after.isNull() ? !value.value("afterEntryId").isNull() : value.value("afterEntryId") != after)) {
                setProblem(tr("The queue changed. Refresh it before moving this entry."));
                co_return false;
            }
        }
    }
    QPointer<RemoteTargetsController> guard(this);
    m_commandBusy = true;
    ++m_commandEpoch; // even an already-running HTTP read is now pre-command
    m_readAgain = false;
    setProblem({});
    const auto authoritative = state();
    beginOptimistic(value);
    emit busyChanged();
    try {
        const auto result = co_await m_hub->remoteCommand(selected.targetId, value, authoritative, CommandScope);
        if (!guard || !isCurrent(selected))
            co_return false;
        if (result.contains("commandSequence"))
            m_ackSequence = result.value("commandSequence").toULongLong();
        m_commandBusy = false;
        emit busyChanged();
        requestState();
        if (m_queueVisible && (name.startsWith("queue") || name == "play"))
            requestQueuePage(true);
        co_return true;
    } catch (const std::exception& error) {
        if (!guard || !isCurrent(selected))
            co_return false;
        m_commandBusy = false;
        m_optimistic.clear();
        m_ackSequence.reset();
        emit busyChanged();
        if (gone(QString::fromUtf8(error.what()))) {
            failTarget(selected, tr("The target is unavailable. Playback remains on its current device."));
        } else {
            setProblem(QString::fromUtf8(error.what()).contains("mixed_source_queue")
                    ? tr("A remote queue can only contain media from the selected target's account.")
                    : tr("The remote command could not be confirmed. Refresh before trying again."));
            requestState();
            if (m_queueVisible && (name.startsWith("queue") || name == "play"))
                requestQueuePage(true); // uncertain mutation: read, never retry
        }
        co_return false;
    }
}
void RemoteTargetsController::send(QVariantMap value)
{
    Async::runScoped(
        this, command(m_selection, std::move(value)), [](bool) {},
        [this](const std::exception_ptr&) { setProblem(tr("The remote command could not be sent.")); },
        "remote target command");
}
QCoro::Task<bool> RemoteTargetsController::play(
    Selection selected, QStringList ids, int index, QString position, QString mode, QString variant)
{
    if (!isCurrent(selected) || selected.targetId.isEmpty())
        co_return false;
    // Validate ownership before any provider request, including metadata reads
    // a caller may otherwise perform after dispatch.
    for (const auto& id : ids)
        if (m_hub->accountOf(id) != selected.accountId) {
            setProblem(tr("A remote queue can only contain media from the selected target's account."));
            co_return false;
        }
    QVariantMap value { { "action", "play" }, { "itemIds", ids }, { "index", index }, { "positionTicks", position },
        { "mode", mode } };
    if (!variant.isEmpty())
        value.insert("variantId", variant);
    co_return co_await command(std::move(selected), std::move(value));
}

void RemoteTargetsController::requestQueuePage(bool reset)
{
    if (!m_foreground || !m_queueVisible || m_selection.targetId.isEmpty() || m_connecting)
        return;
    if (m_queueBusy) {
        m_queueResetPending = m_queueResetPending || reset;
        return;
    }
    if (!reset && m_queueExhausted)
        return;
    m_queueBusy = true;
    m_queueResetPending = false;
    m_lastQueueRead = m_clock.elapsed();
    const auto selected = m_selection;
    const auto generation = ++m_queueGeneration;
    emit queueChanged();
    Async::runScoped(
        this, loadQueue(selected, reset, generation),
        [this, selected, generation] {
            if (!isCurrent(selected) || generation != m_queueGeneration)
                return;
            m_queueBusy = false;
            emit queueChanged();
            if (m_queueResetPending)
                requestQueuePage(true);
        },
        [this, selected, generation](const std::exception_ptr&) {
            if (!isCurrent(selected) || generation != m_queueGeneration)
                return;
            clearQueue();
            setProblem(tr("The remote queue could not be refreshed. Retry before changing an entry."));
        },
        "remote target queue");
}
QCoro::Task<void> RemoteTargetsController::loadQueue(Selection selected, bool reset, quint64 generation)
{
    QPointer<RemoteTargetsController> guard(this);
    const auto revision = m_state.value("queueRevision").toString();
    auto cursor = reset ? std::optional<QString>() : m_cursor;
    auto seen = reset ? QSet<QString>() : m_seenCursors;
    int pages = reset ? 0 : m_queuePages;
    QVariantList rows = reset ? QVariantList() : m_queue;
    QSet<QString> entries;
    for (const auto& row : rows)
        entries.insert(row.toMap().value("entryId").toString());
    const qsizetype desired = reset ? std::max<qsizetype>(1, m_queue.size()) : m_queue.size() + 1;
    bool exhausted = false;
    do {
        if (pages >= 256 || rows.size() >= 10000)
            throw std::runtime_error("response_limit");
        auto page = co_await m_hub->remoteQueue(selected.targetId, cursor, QueueScope);
        if (!guard || !isCurrent(selected) || generation != m_queueGeneration)
            co_return;
        if (revision != m_state.value("queueRevision").toString()) {
            m_queueResetPending = true;
            co_return;
        }
        ++pages;
        if (!page.exhausted && (!page.nextCursor || page.nextCursor->isEmpty() || seen.contains(*page.nextCursor)))
            throw std::runtime_error("invalid_pagination");
        if (!page.exhausted)
            seen.insert(*page.nextCursor);
        for (const auto& item : page.items) {
            if (item.playlistItemId.isEmpty() || entries.contains(item.playlistItemId))
                throw std::runtime_error("invalid_remote_queue");
            entries.insert(item.playlistItemId);
            rows.append(m_hub->remoteQueueRow(item));
        }
        if (rows.size() > 10000)
            throw std::runtime_error("response_limit");
        cursor = page.exhausted ? std::nullopt : page.nextCursor;
        exhausted = page.exhausted;
    } while (!exhausted && rows.size() < desired);
    m_queue = std::move(rows);
    m_cursor = std::move(cursor);
    m_seenCursors = std::move(seen);
    m_queuePages = pages;
    m_queueExhausted = exhausted;
    m_loadedRevision = revision;
}

QObject *RemoteTargetsController::createAdvancedControls()
{
    if (m_selection.targetId.isEmpty() || m_connecting || !m_target.value("customControls").toBool())
        return nullptr;
    if (m_advancedControls && !m_advancedControls->closed())
        return m_advancedControls;
    auto *context = qobject_cast<ProviderUiContext *>(m_registry->openPicker(m_selection.accountId,
        { { "kind", "remoteControls" }, { "targetId", SourceHub::rawId(m_selection.targetId) } }));
    if (!context) {
        setProblem(tr("Advanced controls are unavailable for this target."));
        return nullptr;
    }
    m_advancedControls = context;
    return context;
}

void RemoteTargetsController::openAdvancedControls()
{
    auto *context = qobject_cast<ProviderUiContext *>(createAdvancedControls());
    if (!context)
        return;
    const auto selected = m_selection;
    QPointer<ProviderUiContext> picker(context);
    QTimer::singleShot(0, this, [this, selected, picker] {
        if (!picker)
            return;
        if (!isCurrent(selected) || picker->closed()) {
            picker->close();
            return;
        }
        emit m_registry->componentRequested(picker);
    });
}
void RemoteTargetsController::updateMediaSession()
{
    if (m_selection.targetId.isEmpty() || m_state.isEmpty()) {
        m_mediaSession->clear();
        return;
    }
    RemoteMediaSessionState session;
    session.title = m_state.value("title").toString();
    const auto item = m_state.value("item").toMap();
    session.artist = item.value("albumArtist").toString();
    session.album = item.value("album").toString();
    session.targetName = m_target.value("name").toString();
    session.positionMs = m_position.toLongLong() / 10000;
    session.durationMs = m_runtime.toLongLong() / 10000;
    session.playing = m_state.value("state") == "playing";
    session.playbackRate = m_state.value("rate", 1.0).toDouble();
    session.canSeek = m_state.value("commands").toStringList().contains("seek");
    if (m_state.contains("volume"))
        session.volume = m_state.value("volume").toInt();
    m_mediaSession->update(session);
}
} // namespace Spool

namespace Spool {
void RemoteTargetsController::setTrickplayService(TrickplayService *service)
{
    m_trickplay = service;
    if (service) {
        service->setImageProviderName(QStringLiteral("remote-trickplay"));
        connect(service, &TrickplayService::changed, this, &RemoteTargetsController::trickplayChanged);
    }
}
QVariantMap RemoteTargetsController::trickplayForSeconds(double seconds) const
{
    return m_trickplay ? m_trickplay->frame(seconds) : QVariantMap { { "available", false } };
}
bool RemoteTargetsController::trickplayAvailable() const
{
    return m_trickplay && m_trickplay->available();
}
} // namespace Spool
