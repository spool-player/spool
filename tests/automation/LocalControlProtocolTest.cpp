#include "automation/LocalControlProtocol.h"
#include "TestMain.h"
#include "TestRequire.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <thread>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {
using SpoolTests::require;
QString code(const QJsonObject& response)
{
    return response.value(QStringLiteral("error")).toObject().value(QStringLiteral("code")).toString();
}
}

SPOOL_TEST_MAIN("spoolet-protocol")
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "create private test directory");
#ifdef Q_OS_UNIX
    QTemporaryDir runtime(QStringLiteral("/tmp/spool-protocol-runtime-XXXXXX"));
    require(runtime.isValid(), "create private short test runtime");
    qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
#endif
    QString dataRoot = temporary.path();
#ifdef Q_OS_UNIX
    dataRoot += QStringLiteral("/data-") + QString(80, QLatin1Char('a')) + QStringLiteral("/nested-")
        + QString(80, QLatin1Char('b'));
#endif
    require(QDir().mkpath(dataRoot), "create long selected data root");
    qputenv("SPOOL_DATA_HOME", dataRoot.toUtf8());
    QString error;
    const QString registry = Spool::LocalControl::directory(&error);
    require(!registry.isEmpty() && registry.startsWith(dataRoot), "descriptor registry honors selected data root");
    const QString path = QDir(registry).filePath(QStringLiteral("test.json"));
    QString endpoint = Spool::LocalControl::localEndpoint(registry, QStringLiteral("test"), &error);
#ifndef Q_OS_UNIX
    endpoint += QLatin1Char('-') + QString(32, QLatin1Char('a'));
#endif
    require(!endpoint.isEmpty(), "resolve bounded native socket address");
#ifdef Q_OS_UNIX
    require(QFile::encodeName(registry).size() > 108 && QFileInfo(endpoint).absolutePath() != registry,
        "long registry paths do not become UNIX socket addresses");
#endif
    QJsonObject descriptor { { QStringLiteral("instance"), QStringLiteral("test") },
        { QStringLiteral("endpoint"), endpoint }, { QStringLiteral("token"), QString(64, QLatin1Char('a')) } };
    const auto save = [&] {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "write discovery fixture");
        require(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner), "make discovery owner-only");
        const QByteArray bytes = QJsonDocument(descriptor).toJson(QJsonDocument::Compact);
        require(file.write(bytes) == bytes.size(), "complete discovery write");
    };
    save();
    require(!Spool::LocalControl::readDescriptor(path).isEmpty(), "private valid descriptor is discoverable");
#ifdef Q_OS_UNIX
    require(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadOther),
        "make negative-control descriptor public");
    require(Spool::LocalControl::readDescriptor(path).isEmpty(), "public capability files must not be discoverable");
    save();
    const QString link = QDir(temporary.path()).filePath(QStringLiteral("alias.json"));
    require(symlink(QFile::encodeName(path).constData(), QFile::encodeName(link).constData()) == 0,
        "create symlink fixture");
    require(Spool::LocalControl::readDescriptor(link).isEmpty(), "symlink discovery must not be followed");
    descriptor.insert(QStringLiteral("endpoint"), QStringLiteral("/tmp/s-outside-private-directory"));
#else
    descriptor.insert(QStringLiteral("endpoint"), QStringLiteral("unrelated-pipe"));
#endif
    save();
    require(Spool::LocalControl::readDescriptor(path).isEmpty(), "discovery cannot redirect to an unrelated endpoint");
    descriptor.insert(QStringLiteral("endpoint"), endpoint);
    const QString otherRegistry = temporary.filePath(QStringLiteral("other-registry"));
    require(QDir().mkdir(otherRegistry), "create independent registry");
    QString foreignEndpoint = Spool::LocalControl::localEndpoint(otherRegistry, QStringLiteral("test"), &error);
#ifndef Q_OS_UNIX
    foreignEndpoint += QLatin1Char('-') + QString(32, QLatin1Char('a'));
#endif
    require(!foreignEndpoint.isEmpty() && foreignEndpoint != endpoint,
        "the same instance in another registry has an independent address");
    descriptor.insert(QStringLiteral("endpoint"), foreignEndpoint);
    save();
    require(
        Spool::LocalControl::readDescriptor(path).isEmpty(), "descriptor cannot redirect to another registry's socket");
    descriptor.insert(QStringLiteral("endpoint"), endpoint);
    descriptor.insert(QStringLiteral("transport"), QStringLiteral("tcp"));
    descriptor.insert(QStringLiteral("endpoint"), QStringLiteral("127.0.0.1"));
    descriptor.insert(QStringLiteral("port"), 43210);
    save();
    require(!Spool::LocalControl::readDescriptor(path).isEmpty(), "private loopback TCP descriptor is discoverable");
    descriptor.insert(QStringLiteral("endpoint"), QStringLiteral("192.0.2.1"));
    save();
    require(Spool::LocalControl::readDescriptor(path).isEmpty(),
        "TCP discovery cannot exfiltrate its capability to a remote host");
    descriptor.insert(QStringLiteral("endpoint"), QStringLiteral("127.0.0.1"));
    for (const int port : { -1, 0, 65536 }) {
        descriptor.insert(QStringLiteral("port"), port);
        save();
        require(Spool::LocalControl::readDescriptor(path).isEmpty(), "invalid TCP port cannot be discovered");
    }
    descriptor.insert(QStringLiteral("port"), 43210);
    descriptor.insert(QStringLiteral("transport"), QStringLiteral("https"));
    save();
    require(Spool::LocalControl::readDescriptor(path).isEmpty(), "unknown descriptor transport is rejected");
    descriptor.remove(QStringLiteral("transport"));
    descriptor.remove(QStringLiteral("port"));
    descriptor.insert(QStringLiteral("endpoint"), endpoint);
    require(code(Spool::LocalControl::request(descriptor, QStringLiteral("status"),
                { { QStringLiteral("large"), QString(70 * 1024, QLatin1Char('x')) } }, 100))
            == QStringLiteral("request_too_large"),
        "oversized request rejected before transport access");

    // Exercise both actual transports: peers may fragment frames, lie about
    // request identity, or remain silent. Neither transport may hang the CLI.
    for (const bool tcpTransport : { false, true }) {
        for (int mode = 0; mode < 3; ++mode) {
            std::promise<quint16> listening;
            auto ready = listening.get_future();
            std::thread peer([&listening, endpoint, mode, tcpTransport] {
                QLocalServer local;
                QTcpServer tcp;
                local.setSocketOptions(QLocalServer::UserAccessOption);
                const bool started = tcpTransport ? tcp.listen(QHostAddress::LocalHost, 0) : local.listen(endpoint);
                listening.set_value(started ? (tcpTransport ? tcp.serverPort() : 1) : 0);
                if (!started || !(tcpTransport ? tcp.waitForNewConnection(1500) : local.waitForNewConnection(1500)))
                    return;
                std::unique_ptr<QIODevice> socket(tcpTransport
                        ? static_cast<QIODevice *>(tcp.nextPendingConnection())
                        : static_cast<QIODevice *>(local.nextPendingConnection()));
                QByteArray bytes;
                while (!bytes.contains('\n')) {
                    if (socket->bytesAvailable() == 0 && !socket->waitForReadyRead(1000))
                        return;
                    bytes += socket->readAll();
                }
                const QJsonObject request = QJsonDocument::fromJson(bytes.trimmed()).object();
                if (mode == 2) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    return;
                }
                const QJsonObject response { { QStringLiteral("id"),
                                                 mode == 1 ? QStringLiteral("wrong-request")
                                                           : request.value(QStringLiteral("id")).toString() },
                    { QStringLiteral("ok"), true },
                    { QStringLiteral("result"), QJsonObject { { QStringLiteral("framed"), true } } } };
                const QByteArray frame = QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n';
                socket->write(frame.left(frame.size() / 2));
                socket->waitForBytesWritten(1000);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                socket->write(frame.mid(frame.size() / 2));
                socket->waitForBytesWritten(1000);
                socket->close();
            });
            const quint16 port = ready.get();
            require(port != 0, "start real private local or TCP peer");
            QJsonObject peerDescriptor = descriptor;
            if (tcpTransport) {
                peerDescriptor.insert(QStringLiteral("transport"), QStringLiteral("tcp"));
                peerDescriptor.insert(QStringLiteral("endpoint"), QStringLiteral("127.0.0.1"));
                peerDescriptor.insert(QStringLiteral("port"), port);
            }
            QElapsedTimer elapsed;
            elapsed.start();
            const QJsonObject reply
                = Spool::LocalControl::request(peerDescriptor, QStringLiteral("status"), {}, mode == 2 ? 80 : 1000);
            const qint64 duration = elapsed.elapsed();
            peer.join();
            if (mode == 0)
                require(reply.value(QStringLiteral("ok")).toBool()
                        && reply.value(QStringLiteral("result")).toObject().value(QStringLiteral("framed")).toBool(),
                    "fragmented newline frame is reassembled over each transport");
            else if (mode == 1)
                require(code(reply) == QStringLiteral("invalid_response"), "mismatched request ID is rejected");
            else
                require(code(reply) == QStringLiteral("timeout") && duration < 1000, "silent peer cannot hang the CLI");
        }
    }
    return 0;
}
