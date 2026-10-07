#include "JellyfinFixture.h"

#include <QBuffer>
#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkProxy>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <memory>
#include <utility>

namespace AppJourney {
namespace {
const QString userId = QStringLiteral("journey-user");
const QString movieId = QStringLiteral("journey-movie");
const QByteArray token = "journey-loopback-token";
void respond(QTcpSocket *socket, int status, QByteArray bytes, const QByteArray& type = "application/json",
    QByteArray extra = {}, bool head = false)
{
    QByteArray header = "HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\nConnection: close\r\n"
        "Cache-Control: no-store\r\nContent-Type: " + type + "\r\nContent-Length: " + QByteArray::number(bytes.size())
        + "\r\n" + extra + "\r\n";
    socket->write(header);
    if (!head)
        socket->write(bytes);
    socket->disconnectFromHost();
}
}

JellyfinFixture::JellyfinFixture(QByteArray bytes) : media(std::move(bytes))
{
    QImage poster(240, 360, QImage::Format_RGB32);
    poster.fill(QColor(245, 150, 20));
    for (int y = 180; y < 360; ++y)
        for (int x = 0; x < 240; ++x)
            poster.setPixelColor(x, y, QColor(180, 20, 200));
    QBuffer buffer(&artwork);
    buffer.open(QIODevice::WriteOnly);
    poster.save(&buffer, "PNG");
    server.setProxy(QNetworkProxy::NoProxy);
    QObject::connect(&server, &QTcpServer::newConnection, &server, [this] { accept(); });
    server.listen(QHostAddress::LocalHost, 0);
}
QString JellyfinFixture::origin() const
{
    return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
}
QJsonObject JellyfinFixture::source() const
{
    return { { "Id", "journey-source" }, { "Name", "Original" }, { "Container", "mkv" },
        { "Protocol", "File" }, { "VideoType", "VideoFile" }, { "RunTimeTicks", 300000000 },
        { "Size", media.size() }, { "Bitrate", 128000 }, { "SupportsDirectPlay", true },
        { "SupportsDirectStream", true }, { "SupportsTranscoding", false },
        { "MediaStreams", QJsonArray { QJsonObject { { "Index", 0 }, { "Type", "Video" }, { "Codec", "ffv1" },
            { "Width", 640 }, { "Height", 360 }, { "RealFrameRate", 10 }, { "BitRate", 128000 } } } } };
}
QJsonObject JellyfinFixture::movie() const
{
    return { { "Id", movieId }, { "Name", "Journey Film" }, { "SortName", "Journey Film" },
        { "Type", "Movie" }, { "MediaType", "Video" }, { "IsFolder", false }, { "LocationType", "FileSystem" },
        { "RunTimeTicks", 300000000 }, { "ProductionYear", 2026 },
        { "Overview", "A finite red then green picture above a blue lower half." },
        { "ImageTags", QJsonObject { { "Primary", "journey-poster" } } },
        { "UserData", QJsonObject { { "PlaybackPositionTicks", 0 }, { "Played", false } } },
        { "MediaSources", QJsonArray { source() } } };
}
void JellyfinFixture::accept()
{
    while (auto *socket = server.nextPendingConnection()) {
        auto pending = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QTimer::singleShot(10000, socket, [socket] { socket->abort(); });
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, pending] {
            *pending += socket->readAll();
            if (pending->size() > 1024 * 1024) {
                unexpected.append("oversized fixture request");
                socket->abort();
                return;
            }
            const qsizetype end = pending->indexOf("\r\n\r\n");
            if (end < 0 || socket->property("handled").toBool())
                return;
            qint64 length = 0;
            for (const auto& line : pending->left(end).split('\n')) {
                if (line.toLower().startsWith("content-length:"))
                    length = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
            }
            if (length < 0 || length > 1024 * 1024) {
                unexpected.append("invalid fixture content length");
                socket->abort();
                return;
            }
            if (pending->size() < end + 4 + length)
                return;
            socket->setProperty("handled", true);
            handle(socket, pending->left(end), pending->mid(end + 4, length));
        });
    }
}
void JellyfinFixture::handle(QTcpSocket *socket, const QByteArray& header, const QByteArray& body)
{
    const auto request = header.left(header.indexOf("\r\n")).split(' ');
    if (request.size() != 3) {
        unexpected.append("invalid HTTP request");
        respond(socket, 400, "{}");
        return;
    }
    const QByteArray method = request[0];
    const QUrl url = QUrl::fromEncoded(request[1]);
    const QString path = url.path();
    const QUrlQuery query(url);
    const auto json = [&](const QJsonValue& value, int status = 200) {
        const QByteArray encoded = value.isObject() ? QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)
            : value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
            : value.isBool() ? (value.toBool() ? QByteArray("true") : QByteArray("false")) : QByteArray("{}");
        respond(socket, status, encoded);
    };
    if (method == "GET" && (path == "/official.json" || path == "/index.json")) {
        json(QJsonObject { { "providers", QJsonArray {} } });
        return;
    }
    if (method == "GET" && path == "/System/Info/Public") {
        json(QJsonObject { { "Id", "journey-server" }, { "ServerName", "Journey Server" }, { "Version", "10.10.0" } });
        return;
    }
    if (method == "GET" && path == "/Users/Public") {
        json(QJsonArray {});
        return;
    }
    if (method == "GET" && path == "/QuickConnect/Enabled") {
        json(false);
        return;
    }
    if (method == "POST" && path == "/Users/AuthenticateByName") {
        const auto credentials = QJsonDocument::fromJson(body).object();
        if (credentials["Username"] != "journey" || credentials["Pw"] != "journey-password") {
            ++rejectedLogins;
            json(QJsonObject {}, 401);
        } else {
            ++successfulLogins;
            json(QJsonObject { { "AccessToken", QString::fromLatin1(token) }, { "ServerId", "journey-server" },
                { "User", QJsonObject { { "Id", userId }, { "Name", "Journey Viewer" } } } });
        }
        return;
    }
    // Jellyfin's tag-addressed artwork endpoint is public; library and media
    // endpoints below still require the actual signed-in token.
    if (method == "GET" && path == "/Items/" + movieId + "/Images/Primary"
        && query.queryItemValue("tag") == "journey-poster") {
        respond(socket, 200, artwork, "image/png");
        return;
    }
    if (path == "/socket") {
        respond(socket, 404, "{}"); // No live events are required by this finite fixture.
        return;
    }
    bool authenticated = false;
    QByteArray range;
    for (const auto& line : header.split('\n')) {
        if (line.toLower().startsWith("authorization:"))
            authenticated = line.contains("Token=\"" + token + "\"");
        if (line.toLower().startsWith("range:"))
            range = line.mid(line.indexOf(':') + 1).trimmed();
    }
    if (!authenticated) {
        unexpected.append("missing authenticated request at " + path);
        json(QJsonObject {}, 401);
        return;
    }
    if (method == "GET" && path == "/Users/" + userId + "/Views") {
        ++authenticatedViews;
        json(QJsonObject { { "Items", QJsonArray { QJsonObject { { "Id", "journey-library" },
            { "Name", "Journey Library" }, { "CollectionType", "movies" } } } }, { "TotalRecordCount", 1 } });
    } else if (method == "GET" && (path == "/Items" || path == "/Users/" + userId + "/Items")) {
        if (query.queryItemValue("ParentId") == "journey-library")
            ++authenticatedBrowse;
        json(QJsonObject { { "Items", QJsonArray { movie() } }, { "TotalRecordCount", 1 } });
    } else if (method == "GET" && path == "/Users/" + userId + "/Items/" + movieId) {
        json(movie());
    } else if (method == "GET" && path == "/Users/" + userId + "/Items/Latest") {
        json(QJsonArray { movie() });
    } else if (method == "GET" && (path == "/Users/" + userId + "/Items/Resume" || path == "/Shows/NextUp"
        || path == "/MediaSegments/" + movieId || path == "/Videos/journey-source/AdditionalParts"
        || path == "/Items/" + movieId + "/Similar")) {
        json(QJsonObject { { "Items", QJsonArray {} }, { "TotalRecordCount", 0 } });
    } else if (method == "GET" && path == "/Items/Filters") {
        json(QJsonObject { { "Genres", QJsonArray {} }, { "Years", QJsonArray {} } });
    } else if (method == "GET" && path == "/System/Endpoint") {
        json(QJsonObject { { "IsLocal", true }, { "IsInNetwork", true } });
    } else if (method == "GET" && path == "/Users/" + userId) {
        json(QJsonObject { { "Id", userId }, { "Name", "Journey Viewer" },
            { "Policy", QJsonObject { { "EnableContentDownloading", true }, { "EnableMediaPlayback", true },
                { "EnableUserPreferenceAccess", true }, { "EnableVideoPlaybackTranscoding", false },
                { "EnableAudioPlaybackTranscoding", false }, { "EnableCollectionManagement", false },
                { "EnableContentDeletion", false }, { "EnablePlaylistAccess", false } } },
            { "Configuration", QJsonObject { { "AudioLanguagePreference", "" }, { "SubtitleLanguagePreference", "" },
                { "PlayDefaultAudioTrack", true }, { "SubtitleMode", "Default" } } } });
    } else if (method == "POST" && path == "/Items/" + movieId + "/PlaybackInfo") {
        const auto negotiation = QJsonDocument::fromJson(body).object();
        if (negotiation["UserId"] != userId || !negotiation["EnableDirectPlay"].toBool())
            unexpected.append("invalid playback negotiation");
        ++playbackNegotiations;
        json(QJsonObject { { "PlaySessionId", "journey-session" }, { "MediaSources", QJsonArray { source() } } });
    } else if ((method == "GET" || method == "HEAD") && path == "/Videos/" + movieId + "/stream") {
        ++mediaRequests;
        if (!mediaAvailable) {
            json(QJsonObject {}, 503);
            return;
        }
        qint64 first = 0;
        qint64 last = media.size() - 1;
        if (!range.isEmpty()) {
            if (!range.startsWith("bytes=") || range.contains(',')) {
                respond(socket, 416, "{}");
                return;
            }
            const auto bounds = range.mid(6).split('-');
            bool valid = false;
            if (bounds.size() != 2 || bounds[0].isEmpty()) {
                respond(socket, 416, "{}");
                return;
            }
            first = bounds[0].toLongLong(&valid);
            if (!valid || first < 0 || first >= media.size()) {
                respond(socket, 416, "{}");
                return;
            }
            if (!bounds[1].isEmpty()) {
                last = bounds[1].toLongLong(&valid);
                if (!valid || last < first) {
                    respond(socket, 416, "{}");
                    return;
                }
                last = qMin(last, qint64(media.size() - 1));
            }
        }
        const QByteArray bytes = media.mid(first, last - first + 1);
        if (method == "GET")
            mediaBytes += bytes.size();
        QByteArray extra = "Accept-Ranges: bytes\r\n";
        if (!range.isEmpty())
            extra += "Content-Range: bytes " + QByteArray::number(first) + '-' + QByteArray::number(last) + '/'
                + QByteArray::number(media.size()) + "\r\n";
        respond(socket, range.isEmpty() ? 200 : 206, bytes, "video/x-matroska", extra, method == "HEAD");
    } else if (path.startsWith("/DisplayPreferences/")) {
        if (method == "GET")
            json(documents.value(path, QJsonObject { { "Id", path.mid(path.lastIndexOf('/') + 1) },
                { "CustomPrefs", QJsonObject {} } }));
        else if (method == "POST") {
            documents.insert(path, QJsonDocument::fromJson(body).object());
            json(QJsonObject {});
        } else {
            unexpected.append("unsupported display preference method");
            json(QJsonObject {}, 405);
        }
    } else if (method == "POST" && (path == "/Sessions/Playing" || path == "/Sessions/Playing/Progress"
        || path == "/Sessions/Playing/Stopped")) {
        auto report = QJsonDocument::fromJson(body).object();
        if (report["ItemId"] != movieId || report["PlaySessionId"] != "journey-session")
            unexpected.append("invalid playback report identity");
        report.insert("endpoint", path);
        reports.append(report);
        json(QJsonObject {});
    } else if (method == "POST" && (path == "/Sessions/Capabilities/Full" || path == "/Sessions/Logout")) {
        json(QJsonObject {});
    } else {
        // Unknown traffic is a failure, never a generic success fallback.
        unexpected.append(QString::fromLatin1(method) + ' ' + path);
        json(QJsonObject {}, 404);
    }
}
}
