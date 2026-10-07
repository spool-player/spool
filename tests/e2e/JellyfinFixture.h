#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QMap>
#include <QTcpServer>
#include <QStringList>
#include <QTcpSocket>

// A protocol fixture, not a substitute provider. The shipped Jellyfin package
// performs all login, mapping, playback negotiation and download operations.
namespace AppJourney {
class JellyfinFixture final {
public:
    explicit JellyfinFixture(QByteArray media);
    QString origin() const;
    bool listening() const { return server.isListening(); }
    int successfulLogins = 0;
    int rejectedLogins = 0;
    int authenticatedViews = 0;
    int authenticatedBrowse = 0;
    int playbackNegotiations = 0;
    int mediaRequests = 0;
    qint64 mediaBytes = 0;
    QList<QJsonObject> reports;
    QStringList unexpected;
    void setMediaAvailable(bool available) { mediaAvailable = available; }
    qint64 mediaSize() const { return media.size(); }

private:
    QJsonObject source() const;
    QJsonObject movie() const;
    void accept();
    void handle(QTcpSocket *socket, const QByteArray& header, const QByteArray& body);
    QTcpServer server;
    QByteArray media;
    QByteArray artwork;
    QMap<QString, QJsonObject> documents;
    bool mediaAvailable = true;
};
}
