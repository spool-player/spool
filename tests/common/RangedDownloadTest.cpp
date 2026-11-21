#include "common/RangedDownload.h"

#include "TestMain.h"
#include "TestRequire.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include <optional>

using Spool::RangedDownload;
using SpoolTests::require;

namespace {

// HTTP/1.1, one response per connection. Honours "bytes=a-b" and "bytes=a-"
// unless told to ignore ranges, and records what it was asked for.
class FileServer final : public QTcpServer {
public:
    explicit FileServer(QByteArray body)
        : m_body(std::move(body))
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] { respond(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        require(listen(QHostAddress::LocalHost), "server could not listen");
    }

    bool honourRanges = true;
    QList<QByteArray> ranges;

    QUrl url() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/file").arg(serverPort()));
    }

private:
    void respond(QTcpSocket *socket)
    {
        QByteArray& request = m_pending[socket];
        request += socket->readAll();
        if (!request.contains("\r\n\r\n"))
            return;
        QByteArray range;
        for (const QByteArray& line : request.split('\n')) {
            if (line.toLower().startsWith("range:"))
                range = line.mid(6).trimmed();
        }
        m_pending.remove(socket);
        ranges.append(range);
        QByteArray head;
        QByteArray body = m_body;
        if (honourRanges && range.startsWith("bytes=")) {
            const QList<QByteArray> bounds = range.mid(6).split('-');
            const qint64 first = bounds.at(0).toLongLong();
            const qint64 last = bounds.at(1).isEmpty() ? m_body.size() - 1 : bounds.at(1).toLongLong();
            body = m_body.mid(first, last - first + 1);
            head = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + QByteArray::number(first) + '-'
                + QByteArray::number(last) + '/' + QByteArray::number(m_body.size()) + "\r\n";
        } else {
            head = "HTTP/1.1 200 OK\r\n";
        }
        head += "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
        socket->write(head + body);
        socket->disconnectFromHost();
    }

    QByteArray m_body;
    QHash<QTcpSocket *, QByteArray> m_pending;
};

QByteArray noise(qsizetype size)
{
    QByteArray bytes(size, Qt::Uninitialized);
    QRandomGenerator generator(1234);
    for (char& byte : bytes)
        byte = static_cast<char>(generator.bounded(256));
    return bytes;
}

struct Outcome {
    QString error;
    QByteArray file;
};

Outcome fetch(FileServer& server, const QString& path, int lanes, qint64 expected)
{
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    RangedDownload download(&network, QNetworkRequest(server.url()), path);
    download.setLanes(lanes);
    download.setMinimumLaneBytes(64 * 1024);
    if (expected >= 0)
        download.setExpectedBytes(expected);
    std::optional<QString> result;
    QObject::connect(&download, &RangedDownload::finished, [&](const QString& error) { result = error; });
    download.start();
    QElapsedTimer timer;
    timer.start();
    while (!result && timer.elapsed() < 10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    require(result.has_value(), "download never finished");
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "downloaded file could not be read");
    return { *result, file.readAll() };
}

} // namespace

SPOOL_TEST_MAIN("ranged-download")
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "temporary directory");
    const QByteArray body = noise(3 * 1024 * 1024 + 17);
    const QString path = directory.filePath(QStringLiteral("download.bin"));

    // A known size is split up front; lanes that finish take over the rest.
    {
        FileServer server(body);
        const Outcome outcome = fetch(server, path, 3, body.size());
        require(outcome.error.isEmpty(), "ranged download of a known size failed");
        require(outcome.file == body, "ranged download of a known size reassembled the wrong bytes");
        require(server.ranges.size() >= 3, "a known size was not fetched over several ranges");
        require(!server.ranges.contains(QByteArray()), "a ranged download sent a request without a range");
    }
    // An unknown size is learned from the first answer, then split.
    {
        FileServer server(body);
        const Outcome outcome = fetch(server, path, 2, -1);
        require(outcome.error.isEmpty(), "ranged download of an unknown size failed");
        require(outcome.file == body, "ranged download of an unknown size reassembled the wrong bytes");
        require(server.ranges.first() == "bytes=0-", "an unknown size did not start with an open range");
        require(server.ranges.size() >= 2, "an unknown size was never split once known");
    }
    // A server that ignores ranges is fetched whole, once.
    {
        FileServer server(body);
        server.honourRanges = false;
        const Outcome outcome = fetch(server, path, 3, body.size());
        require(outcome.error.isEmpty(), "a server without ranges failed the download");
        require(outcome.file == body, "a server without ranges produced the wrong bytes");
        require(server.ranges.last().isEmpty(), "the fallback still asked for a range");
    }
    // A file that is not the size it was promised to be is refused.
    {
        FileServer server(body);
        const Outcome outcome = fetch(server, path, 2, body.size() - 1);
        require(!outcome.error.isEmpty(), "a file of the wrong size was accepted");
    }
    return 0;
}
