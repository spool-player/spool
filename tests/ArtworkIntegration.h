#pragma once

#include "app/ArtworkImageProvider.h"
#include "app/ArtworkService.h"
#include "models/MovieGridModel.h"
#include "provider/ArtworkSource.h"
#include <QBuffer>
#include <QImage>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

class ArtworkIntegration final : public Spool::ArtworkSource {
public:
    ArtworkIntegration()
        : artwork(directory.path(), 0, 1024 * 1024, 1, nullptr)
    {
        QImage image(32, 18, QImage::Format_RGB32);
        image.fill(QColor(30, 180, 70));
        QBuffer encoded(&png);
        if (!encoded.open(QIODevice::WriteOnly) || !image.save(&encoded, "PNG"))
            qFatal("Artwork fixture could not encode its image");
        if (!server.listen(QHostAddress::LocalHost))
            qFatal("Artwork fixture could not listen");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    if (socket->property("answered").toBool())
                        return;
                    const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                    if (!request.contains("\r\n\r\n")) {
                        socket->setProperty("request", request);
                        return;
                    }
                    socket->setProperty("answered", true);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: "
                        + QByteArray::number(png.size()) + "\r\nConnection: close\r\n\r\n" + png);
                    socket->disconnectFromHost();
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        Spool::MovieItem item;
        item.id = "01234567:movie";
        item.title = "Example movie";
        item.itemType = "Movie";
        item.posterTag = "poster";
        item.thumbTag = "thumb";
        item.thumbItemId = "01234567:owner";
        item.backdropTag = "backdrop";
        item.backdropItemId = "01234567:backdrop-owner";
        item.seriesId = "01234567:series";
        item.seriesPrimaryImageTag = "series-poster";
        model.setMovies({ item });
        artwork.setSource(this);
        qmlRegisterSingletonInstance("Spool", 1, 0, "Art", &artwork);
    }
    QString imageUrl(const ImageRequest& request) const override
    {
        if ((request.imageType == "Primary" && request.itemId == "01234567:movie" && request.tag == "poster")
            || (request.imageType == "Thumb" && request.itemId == "01234567:owner" && request.tag == "thumb")
            || (request.imageType == "Backdrop" && request.itemId == "01234567:backdrop-owner"
                && request.tag == "backdrop")
            || (request.imageType == "Primary" && request.itemId == "01234567:series"
                && request.tag == "series-poster"))
            return QStringLiteral("http://127.0.0.1:%1/green.png?kind=%2&owner=%3")
                .arg(server.serverPort())
                .arg(request.imageType, request.itemId);
        return {};
    }
    void expose(QQmlEngine *engine)
    {
        engine->rootContext()->setContextProperty("ArtworkModel", &model);
        engine->addImageProvider("artwork", new Spool::ArtworkImageProvider(&artwork));
    }
    QTemporaryDir directory;
    Spool::MovieGridModel model;
    Spool::ArtworkService artwork;
    QTcpServer server;
    QByteArray png;
};
