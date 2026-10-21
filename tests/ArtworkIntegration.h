#pragma once

#include "app/ArtworkImageProvider.h"
#include "app/ArtworkService.h"
#include "app/BrowseSessionController.h"
#include "models/MovieGridModel.h"
#include "provider/ArtworkSource.h"
#include <QBuffer>
#include <QImage>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <memory>
#include <vector>

class ArtworkIntegration final : public QObject, public Spool::ArtworkSource {
    Q_OBJECT
    Q_PROPERTY(QStringList requestedItems MEMBER requestedItems NOTIFY requestsChanged)
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
                    const auto target = QUrl(QString::fromUtf8(request.split(' ').value(1)));
                    requestedItems.push_back(QUrlQuery(target).queryItemValue("owner"));
                    emit requestsChanged();
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
            || (request.imageType == "Primary" && request.itemId == "01234567:series" && request.tag == "series-poster")
            || (request.imageType == "Primary" && request.itemId.startsWith("01234567:library-")))
            return QStringLiteral("http://127.0.0.1:%1/green.png?kind=%2&owner=%3")
                .arg(server.serverPort())
                .arg(request.imageType, request.itemId);
        return {};
    }
    Q_INVOKABLE void prepareLibrary(QObject *browse, QObject *libraries, QObject *window, QObject *settings)
    {
        std::vector<Spool::MovieItem> items;
        for (int i = 0; i < 300; ++i) {
            Spool::MovieItem item;
            item.id = QStringLiteral("01234567:library-%1").arg(i);
            item.title = QStringLiteral("Movie %1").arg(i);
            item.itemType = "Movie";
            item.posterTag = "poster";
            items.push_back(std::move(item));
        }
        model.setMovies(items);
        requestedItems.clear();
        engine->rootContext()->setContextProperty("Browse", browse);
        engine->rootContext()->setContextProperty("Libraries", libraries);
        engine->rootContext()->setContextProperty("NativeWindow", window);
        engine->rootContext()->setContextProperty("Settings", settings);
    }
    // A name-sorted library of `rows` titles behind a server that answers
    // each block `latencyMs` later, through the real browse controller: the
    // sparse model, block scheduling, letter index and seek the page uses.
    Q_INVOKABLE QObject *prepareSparseLibrary(
        QObject *libraries, QObject *window, QObject *settings, int rows, int latencyMs)
    {
        sparseTitles.clear();
        for (int i = 0; i < rows; ++i) {
            // Skewed like a real library: lots of S, T and M, few Q and X.
            static const char *const starts[] = { "S", "S", "T", "M", "B", "A", "C", "D", "H", "L", "P", "R", "W", "F",
                "G", "N", "K", "J", "E", "I", "O", "V", "Y", "Z", "U", "1" };
            const QString title = QString::fromLatin1(starts[(i * 7919) % 26])
                + QStringLiteral("ample %1").arg((i * 104729) % 1000003, 7, 10, QLatin1Char('0'));
            sparseTitles.push_back(title);
        }
        std::sort(sparseTitles.begin(), sparseTitles.end(),
            [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
        sparse = std::make_unique<Spool::BrowseSessionController>(nullptr);
        // Returned to QML below: without this the engine would own, and
        // delete, what the unique_ptr also deletes.
        QQmlEngine::setObjectOwnership(sparse.get(), QQmlEngine::CppOwnership);
        QQmlEngine::setObjectOwnership(sparse->items(), QQmlEngine::CppOwnership);
        blockRequests = 0;
        QObject::connect(
            sparse.get(), &Spool::BrowseSessionController::blockRequested, this, [this, latencyMs](int start) {
                ++blockRequests;
                QTimer::singleShot(latencyMs, this,
                    [this, start] { sparse->applyBlock(start, sparsePage(start), sparse->cacheKey()); });
            });
        QObject::connect(sparse.get(), &Spool::BrowseSessionController::indexRequested, this, [this, latencyMs] {
            QTimer::singleShot(latencyMs, this, [this] { sparse->setExactIndex({}, sparse->cacheKey()); });
        });
        Spool::LibraryItem library;
        library.id = QStringLiteral("01234567:library");
        library.name = QStringLiteral("Films");
        library.collectionType = QStringLiteral("movies");
        sparse->enterLibrary(library, { { QStringLiteral("sortBy"), QStringLiteral("SortName") } });
        sparse->setPage(sparsePage(0), QStringLiteral("profile:library"), false);
        requestedItems.clear();
        engine->rootContext()->setContextProperty("Browse", sparse.get());
        engine->rootContext()->setContextProperty("Libraries", libraries);
        engine->rootContext()->setContextProperty("NativeWindow", window);
        engine->rootContext()->setContextProperty("Settings", settings);
        return sparse.get();
    }
    Q_INVOKABLE int sparseBlockRequests() const
    {
        return blockRequests;
    }
    Q_INVOKABLE bool sparseRowLoaded(int row) const
    {
        return sparse && sparse->items()->isLoaded(row);
    }
    Spool::PagedMovieItems sparsePage(int start) const
    {
        Spool::PagedMovieItems page;
        page.startIndex = start;
        page.limit = 100;
        page.totalRecordCount = static_cast<int>(sparseTitles.size());
        page.seekable = true;
        const int end = std::min(page.totalRecordCount, start + 100);
        for (int row = start; row < end; ++row) {
            Spool::MovieItem item;
            item.id = QStringLiteral("01234567:library-%1").arg(row);
            item.title = sparseTitles[static_cast<size_t>(row)];
            item.itemType = QStringLiteral("Movie");
            item.posterTag = QStringLiteral("poster");
            page.items.push_back(std::move(item));
        }
        page.exhausted = end >= page.totalRecordCount;
        if (!page.exhausted)
            page.nextCursor = QString::number(end);
        return page;
    }
    void expose(QQmlEngine *engine)
    {
        engine->rootContext()->setContextProperty("ArtworkModel", &model);
        this->engine = engine;
        engine->rootContext()->setContextProperty("ArtworkFixture", this);
        engine->addImageProvider("artwork", new Spool::ArtworkImageProvider(&artwork));
    }
signals:
    void requestsChanged();

public:
    QQmlEngine *engine = nullptr;
    QStringList requestedItems;
    QTemporaryDir directory;
    Spool::MovieGridModel model;
    Spool::ArtworkService artwork;
    QTcpServer server;
    QByteArray png;
    std::vector<QString> sparseTitles;
    std::unique_ptr<Spool::BrowseSessionController> sparse;
    int blockRequests = 0;
};
