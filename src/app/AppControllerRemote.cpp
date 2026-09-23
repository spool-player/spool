// Remote commands and group playback glue for AppController, kept apart from
// the browsing and queue logic in AppController.cpp.
#include "AppController.h"

#include "../common/AsyncTask.h"
#include "../player/PlayQueueController.h"
#include "../player/PlayerController.h"
#include "../provider/SourceHub.h"
#include "GroupPlaybackController.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QWindow>

namespace Spool {

bool AppController::inGroup() const
{
    return m_group && m_group->enabled();
}

void AppController::groupOrLocalTogglePause()
{
    if (inGroup())
        m_group->requestTogglePause();
    else
        m_player->togglePause();
}

QString AppController::queueEntryId(int index) const
{
    return m_playQueue->itemAt(index).playlistItemId;
}

bool AppController::enqueueForGroup(const MovieItem& item, bool queueNext)
{
    if (!inGroup())
        return false;
    if (item.id.isEmpty() || !item.isPlayable()) {
        setErrorText(QStringLiteral("This item cannot be queued."));
        return true;
    }
    m_group->requestQueueItems({ item.id }, queueNext);
    return true;
}

// Commands arrive already translated by the provider into one vocabulary
// (see sdk/provider.d.ts, RemoteCommand), so any server's "play this on that
// device" lands here the same way.
void AppController::handleRemoteCommand(const QString& accountId, const QVariantMap& command)
{
    const QString name = command.value(QStringLiteral("command")).toString();
    const auto seconds
        = [&command] { return command.value(QStringLiteral("positionTicks")).toLongLong() / 10'000'000.0; };
    qInfo() << "remote:" << name;
    if (name == QStringLiteral("play")) {
        playRemoteItems(accountId, command);
    } else if (name == QStringLiteral("pause") || name == QStringLiteral("unpause")) {
        if (m_player->paused() == (name == QStringLiteral("unpause")))
            groupOrLocalTogglePause();
    } else if (name == QStringLiteral("playPause")) {
        groupOrLocalTogglePause();
    } else if (name == QStringLiteral("stop")) {
        m_player->stopWithReason(QStringLiteral("remote-stop"));
    } else if (name == QStringLiteral("seek")) {
        inGroup() ? m_group->requestSeek(seconds()) : m_player->seek(seconds());
    } else if (name == QStringLiteral("rewind")) {
        inGroup() ? m_group->requestRelativeSeek(-10.0) : m_player->seekBack();
    } else if (name == QStringLiteral("fastForward")) {
        inGroup() ? m_group->requestRelativeSeek(10.0) : m_player->seekForward();
    } else if (name == QStringLiteral("next")) {
        playQueueNext();
    } else if (name == QStringLiteral("previous")) {
        playQueuePrevious();
    } else if (name == QStringLiteral("volume")) {
        m_player->setVolume(command.value(QStringLiteral("value"), m_player->volume()).toInt());
    } else if (name == QStringLiteral("volumeStep")) {
        m_player->adjustVolume(command.value(QStringLiteral("delta")).toInt());
    } else if (name == QStringLiteral("mute")) {
        m_player->setMuted(command.value(QStringLiteral("value"), true).toBool());
    } else if (name == QStringLiteral("toggleMute")) {
        m_player->toggleMuted();
    } else if (name == QStringLiteral("audioTrack")) {
        m_player->selectAudioStreamIndex(command.value(QStringLiteral("index"), -1).toInt());
    } else if (name == QStringLiteral("subtitleTrack")) {
        m_player->selectSubtitleStreamIndex(command.value(QStringLiteral("index"), -1).toInt());
    } else if (name == QStringLiteral("repeat")) {
        const QString mode = command.value(QStringLiteral("mode")).toString();
        if (mode == QStringLiteral("RepeatNone") || mode == QStringLiteral("RepeatAll")
            || mode == QStringLiteral("RepeatOne"))
            m_repeatMode = mode;
    } else if (name == QStringLiteral("shuffle")) {
        m_playQueue->setShuffled(command.value(QStringLiteral("value")).toBool());
    } else if (name == QStringLiteral("quality")) {
        selectStreamingQuality(
            command.value(QStringLiteral("bitrate")).toLongLong(), command.value(QStringLiteral("height")).toInt());
    } else if (name == QStringLiteral("stats")) {
        m_player->toggleDebugOsd();
    } else if (name == QStringLiteral("navigate")) {
        // home, search, settings, osd, menu or fullscreen: the shell's to do.
        emit remoteUiActionRequested(command.value(QStringLiteral("to")).toString());
    } else if (name == QStringLiteral("show")) {
        const QString itemId = command.value(QStringLiteral("itemId")).toString();
        if (!itemId.isEmpty())
            emit remoteContentRequested(m_provider->scoped(accountId, itemId),
                command.value(QStringLiteral("itemType")).toString(),
                command.value(QStringLiteral("title")).toString());
    } else if (name == QStringLiteral("message")) {
        const QString text = command.value(QStringLiteral("text")).toString().trimmed();
        if (!text.isEmpty())
            emit remoteMessageRequested(text);
    } else if (name == QStringLiteral("text")) {
        const QString text = command.value(QStringLiteral("value")).toString();
        if (QObject *focus = QGuiApplication::focusObject(); focus && !text.isEmpty()) {
            QInputMethodEvent input;
            input.setCommitString(text);
            QCoreApplication::sendEvent(focus, &input);
        }
    } else if (name == QStringLiteral("key")) {
        static const QHash<QString, int> keys { { QStringLiteral("up"), Qt::Key_Up },
            { QStringLiteral("down"), Qt::Key_Down }, { QStringLiteral("left"), Qt::Key_Left },
            { QStringLiteral("right"), Qt::Key_Right }, { QStringLiteral("pageUp"), Qt::Key_PageUp },
            { QStringLiteral("pageDown"), Qt::Key_PageDown }, { QStringLiteral("select"), Qt::Key_Return },
            { QStringLiteral("back"), Qt::Key_Back }, { QStringLiteral("home"), Qt::Key_Home },
            { QStringLiteral("end"), Qt::Key_End }, { QStringLiteral("space"), Qt::Key_Space } };
        const auto key = keys.constFind(command.value(QStringLiteral("name")).toString());
        if (key != keys.cend() && QGuiApplication::focusWindow()) {
            QKeyEvent press(QEvent::KeyPress, *key, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, *key, Qt::NoModifier);
            QCoreApplication::sendEvent(QGuiApplication::focusWindow(), &press);
            QCoreApplication::sendEvent(QGuiApplication::focusWindow(), &release);
        }
    }
}

void AppController::playRemoteItems(const QString& accountId, const QVariantMap& command)
{
    QStringList itemIds;
    for (const QVariant& id : command.value(QStringLiteral("itemIds")).toList())
        itemIds.append(m_provider->scoped(accountId, id.toString()));
    itemIds.removeAll(QString());
    if (itemIds.isEmpty())
        return;
    const QString mode = command.value(QStringLiteral("mode"), QStringLiteral("now")).toString();
    const int requestedIndex = command.value(QStringLiteral("index")).toInt();
    const qint64 startTicks = command.value(QStringLiteral("positionTicks")).toLongLong();
    const RequestGeneration::Token generation = m_remotePlaybackRequestGeneration.next();
    setBusy(true, QStringLiteral("Starting playback…"));
    Async::runScoped(
        this, m_catalog->fetchItemsByIds(itemIds),
        [this, generation, mode, requestedIndex, startTicks](std::vector<MovieItem> items) {
            if (!m_remotePlaybackRequestGeneration.isCurrent(generation))
                return;
            setBusy(false);
            if (items.empty()) {
                showToast(QStringLiteral("That item isn't available here."));
                return;
            }
            if (mode == QStringLiteral("next") || mode == QStringLiteral("last")) {
                m_playQueue->addToQueue(items, mode == QStringLiteral("next"));
                return;
            }
            const int index = std::clamp(requestedIndex, 0, static_cast<int>(items.size()) - 1);
            items[size_t(index)].resumeTicks = std::max<qint64>(0, startTicks);
            if (!m_playQueue->playNow(items, index))
                return;
            if (mode == QStringLiteral("shuffle"))
                m_playQueue->setShuffled(true);
            startQueuedPlayback(startTicks <= 0);
        },
        [this, generation](const std::exception_ptr& error) {
            if (!m_remotePlaybackRequestGeneration.isCurrent(generation))
                return;
            setBusy(false);
            showToast(exceptionMessage(error));
        },
        "remote playback");
}

} // namespace Spool
