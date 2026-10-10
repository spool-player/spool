#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWebSocket>
#include <QWebSocketServer>

#include <utility>

// A protocol fixture, not a substitute provider. The shipped Jellyfin package
// performs all login, mapping, playback negotiation and download operations.
namespace AppJourney {
class JellyfinFixture final {
public:
    explicit JellyfinFixture(QByteArray media);
    QString origin() const;
    bool listening() const
    {
        return server.isListening();
    }
    int successfulLogins = 0;
    int rejectedLogins = 0;
    int authenticatedViews = 0;
    int authenticatedBrowse = 0;
    int authenticatedRequests = 0;
    int playbackNegotiations = 0;
    int mediaRequests = 0;
    qint64 mediaBytes = 0;
    QList<QJsonObject> reports;
    QStringList unexpected;
    int detailRequests = 0;
    int detailResponses = 0;
    int episodeRequests = 0;
    int episodeDetailRequests = 0;
    int episodeDetailResponses = 0;
    void setEpisodeDetailsHeld(bool held);
    bool sendRemotePlay(qint64 positionTicks);
    // Existing idle event transport survives; every new authenticated request is rejected.
    void setNewRequestsAvailable(bool available)
    {
        newRequestsAvailable = available;
    }
    qint64 mediaSize() const
    {
        return media.size();
    }

private:
    QJsonObject source() const;
    QJsonObject movie() const;
    QJsonObject episode() const;
    void accept();
    bool upgradeWebSocket(QTcpSocket *socket);
    void handle(QTcpSocket *socket, const QByteArray& header, const QByteArray& body);
    QTcpServer server;
    QWebSocketServer webSockets;
    QByteArray media;
    QByteArray artwork;
    QMap<QString, QJsonObject> documents;
    bool newRequestsAvailable = true;
    bool episodeDetailsHeld = false;
    QList<std::pair<QPointer<QTcpSocket>, QByteArray>> pendingDetails;
    QList<QPointer<QWebSocket>> eventSockets;
};
}
