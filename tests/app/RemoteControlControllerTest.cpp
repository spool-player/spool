#include "app/RemoteControlController.h"
#include "api/JellyfinApiFacade.h"
#include "common/TlsTrust.h"

#include "TestMain.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>

using JellyfinNative::RemoteControlController;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

QJsonObject session(QString id, QString name, bool controllable, QString title = {}, bool paused = true)
{
    QJsonObject session {
        { QStringLiteral("Id"), id },
        { QStringLiteral("DeviceId"), QStringLiteral("device-") + id },
        { QStringLiteral("DeviceName"), name },
        { QStringLiteral("DeviceType"), QStringLiteral("TV") },
        { QStringLiteral("Client"), QStringLiteral("Spool for Jellyfin") },
        { QStringLiteral("UserName"), QStringLiteral("Sacha") },
        { QStringLiteral("Capabilities"),
            QJsonObject {
                { QStringLiteral("SupportsMediaControl"), controllable },
                { QStringLiteral("SupportedCommands"),
                    QJsonArray { QStringLiteral("SetVolume"), QStringLiteral("MoveUp") } },
            } },
    };
    if (!title.isEmpty()) {
        session.insert(QStringLiteral("NowPlayingItem"),
            QJsonObject {
                { QStringLiteral("Id"), QStringLiteral("item-1") },
                { QStringLiteral("Name"), title },
                { QStringLiteral("Type"), QStringLiteral("Movie") },
                { QStringLiteral("RunTimeTicks"), 1'200'000'000 },
            });
        session.insert(QStringLiteral("PlayState"),
            QJsonObject {
                { QStringLiteral("PositionTicks"), 300'000'000 },
                { QStringLiteral("IsPaused"), paused },
                { QStringLiteral("VolumeLevel"), 37 },
                { QStringLiteral("IsMuted"), false },
            });
    }
    return session;
}

class MemoryReply final : public QNetworkReply {
public:
    MemoryReply(const QNetworkRequest& request, QNetworkAccessManager::Operation operation, QObject *parent)
        : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(operation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    }

    void complete(QByteArray payload)
    {
        m_payload = std::move(payload);
        setFinished(true);
        emit readyRead();
        emit finished();
    }

    void abort() override { }

    qint64 bytesAvailable() const override
    {
        return m_payload.size() - m_offset + QNetworkReply::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        if (m_offset >= m_payload.size() || maxSize <= 0)
            return -1;
        const qint64 length = std::min<qint64>(maxSize, m_payload.size() - m_offset);
        std::memcpy(data, m_payload.constData() + m_offset, static_cast<size_t>(length));
        m_offset += length;
        return length;
    }

private:
    QByteArray m_payload;
    qsizetype m_offset = 0;
};

class FakeNetworkAccessManager final : public QNetworkAccessManager {
public:
    QPointer<MemoryReply> pendingSessions;
    int playbackCommands = 0;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest& request, QIODevice *) override
    {
        auto *reply = new MemoryReply(request, operation, this);
        if (operation == GetOperation && request.url().path() == QStringLiteral("/Sessions")) {
            pendingSessions = reply;
        } else {
            if (request.url().path().contains(QStringLiteral("/Playing")))
                ++playbackCommands;
            QTimer::singleShot(0, reply, [reply]() { reply->complete(QByteArrayLiteral("{}")); });
        }
        return reply;
    }
};

} // namespace

JELLYFIN_TEST_MAIN("remote-control-controller")
{
    QCoreApplication app(argc, argv);
    RemoteControlController remote(nullptr);
    QString announcedTarget;
    QObject::connect(&remote, &RemoteControlController::targetChanged, [&remote, &announcedTarget]() {
        if (remote.targetSelected())
            announcedTarget = remote.selectedTargetName();
    });

    remote.applySessions(QJsonArray {
        session(QStringLiteral("hidden"), QStringLiteral("Phone"), false),
        session(QStringLiteral("living-room"), QStringLiteral("Living Room"), true, QStringLiteral("Arrival")),
    });
    require(remote.targets().size() == 1, "non-controllable session was exposed as a Cast target");
    require(remote.targets().front().toMap().value(QStringLiteral("deviceName")).toString()
            == QStringLiteral("Living Room"),
        "controllable target metadata was not exposed");

    remote.selectTarget(QStringLiteral("living-room"));
    require(remote.targetSelected(), "available target was not selected");
    require(remote.selectedTargetName() == QStringLiteral("Living Room"), "selected target name was not retained");
    require(announcedTarget == QStringLiteral("Living Room"),
        "selecting a target did not announce the connection without requiring navigation");
    require(remote.nowPlayingItem().value(QStringLiteral("title")).toString() == QStringLiteral("Arrival"),
        "selected target now-playing item was not normalized");
    require(remote.positionTicks() == 300'000'000, "selected target position was not applied");
    require(remote.runtimeTicks() == 1'200'000'000, "selected target duration was not applied");
    require(remote.volume() == 37, "selected target volume was not applied");
    require(remote.supports(QStringLiteral("MoveUp")), "selected target commands were not applied");

    remote.togglePause();
    require(!remote.paused(), "play command did not update the remote state immediately");
    remote.applySessions(QJsonArray {
        session(QStringLiteral("living-room"), QStringLiteral("Living Room"), true, QStringLiteral("Arrival"), true),
    });
    require(!remote.paused(), "stale target state reverted an optimistic play command");
    remote.applySessions(QJsonArray {
        session(QStringLiteral("living-room"), QStringLiteral("Living Room"), true, QStringLiteral("Arrival"), false),
    });
    require(!remote.paused(), "confirmed play state was not retained");
    remote.applySessions(QJsonArray {
        session(QStringLiteral("living-room"), QStringLiteral("Living Room"), true, QStringLiteral("Arrival"), true),
    });
    require(remote.paused(), "reported pause state remained stuck after command confirmation");

    QJsonObject other
        = session(QStringLiteral("bedroom"), QStringLiteral("Living Room"), true, QStringLiteral("Dune"), false);
    QJsonObject otherState = other.value(QStringLiteral("PlayState")).toObject();
    otherState.insert(QStringLiteral("PositionTicks"), 100'000'000);
    otherState.insert(QStringLiteral("AudioStreamIndex"), 2);
    other.insert(QStringLiteral("PlayState"), otherState);
    QJsonObject otherItem = other.value(QStringLiteral("NowPlayingItem")).toObject();
    otherItem.insert(QStringLiteral("MediaStreams"),
        QJsonArray {
            QJsonObject {
                { QStringLiteral("Type"), QStringLiteral("Audio") },
                { QStringLiteral("Index"), 2 },
                { QStringLiteral("DisplayTitle"), QStringLiteral("French") },
            },
        });
    other.insert(QStringLiteral("NowPlayingItem"), otherItem);
    remote.applySessions(QJsonArray {
        session(QStringLiteral("living-room"), QStringLiteral("Living Room"), true, QStringLiteral("Arrival"), false),
        other,
    });
    remote.seek(900'000'000);
    remote.togglePause();
    int attachmentAnnouncements = 0;
    const auto attachmentConnection = QObject::connect(&remote, &RemoteControlController::targetChanged, [&]() {
        ++attachmentAnnouncements;
        require(remote.nowPlayingItem().value(QStringLiteral("title")).toString() == QStringLiteral("Dune")
                && remote.positionTicks() == 100'000'000 && !remote.paused(),
            "target attachment announced before adopting its playing state");
    });
    // Both devices play the same item ID and have identical display metadata:
    // device identity, not item/name changes, must discard the pending commands.
    remote.selectTarget(QStringLiteral("bedroom"));
    require(attachmentAnnouncements == 1, "target attachment was not announced exactly once");
    QObject::disconnect(attachmentConnection);
    require(remote.audioTracks().size() == 1
            && remote.audioTracks().front().toMap().value(QStringLiteral("selected")).toBool(),
        "attachment did not adopt the target's selected audio stream");
    remote.seekRelative(10'000'000);
    require(remote.positionTicks() >= 110'000'000 && remote.positionTicks() < 120'000'000,
        "relative seek after attachment used the previous target's pending seek");

    remote.togglePause();
    otherItem.insert(QStringLiteral("Id"), QStringLiteral("item-2"));
    otherItem.insert(QStringLiteral("Name"), QStringLiteral("Next movie"));
    other.insert(QStringLiteral("NowPlayingItem"), otherItem);
    remote.applySessions(QJsonArray { other });
    require(remote.positionTicks() == 100'000'000 && !remote.paused(),
        "a new item inherited the previous item's pending seek or pause");

    remote.applySessions(QJsonArray {});
    require(!remote.targetSelected(), "disappeared target remained selected");
    require(remote.nowPlayingItem().isEmpty(), "disappeared target retained stale playback state");

    FakeNetworkAccessManager network;
    JellyfinNative::TlsTrustController tlsTrust;
    JellyfinNative::JellyfinApiFacade api(&network, &tlsTrust);
    api.setServerUrl(QStringLiteral("http://192.168.1.2"));
    api.setSession(JellyfinNative::AuthSession {
        QStringLiteral("user-1"), QStringLiteral("Tester"), QStringLiteral("token-1"), QStringLiteral("server-1") });
    RemoteControlController connected(&api);
    connected.refreshTargets();
    require(network.pendingSessions, "session refresh did not start");
    connected.applySessions(QJsonArray { other });
    connected.selectTarget(QStringLiteral("bedroom"));
    network.pendingSessions->complete(QByteArrayLiteral("[]"));
    QCoreApplication::processEvents();
    require(!connected.busy(), "superseded session refresh did not finish");
    require(connected.targetSelected()
            && connected.nowPlayingItem().value(QStringLiteral("title")).toString() == QStringLiteral("Next movie")
            && connected.positionTicks() == 100'000'000 && !connected.paused(),
        "late HTTP snapshot overwrote the pushed active session during attachment");
    require(network.playbackCommands == 0, "attaching to an active session sent a playback command");

    connected.refreshTargets();
    require(network.pendingSessions, "second session refresh did not start");
    connected.stop();
    network.pendingSessions->complete(QJsonDocument(QJsonArray { other }).toJson(QJsonDocument::Compact));
    QCoreApplication::processEvents();
    require(connected.targets().isEmpty() && !connected.targetSelected(),
        "a session refresh completed after stop and restored stale devices");
    return EXIT_SUCCESS;
}
