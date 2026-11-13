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
#include <QProcess>
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
#include <cerrno>
#include <csignal>
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
#ifdef Q_OS_UNIX
    // Exercise the same script used by remote CLI discovery and diagnose.sh.
    const QString discoveryPath = temporary.filePath(QStringLiteral("remote.json"));
    QFile discovery(discoveryPath);
    const QByteArray discoveryBytes = QJsonDocument(
        QJsonObject { { "instance", "remote-test" }, { "transport", "tcp" }, { "endpoint", "127.0.0.1" },
            { "port", 12345 }, { "token", QString(64, QLatin1Char('b')) } })
                                          .toJson(QJsonDocument::Compact);
    require(discovery.open(QIODevice::WriteOnly) && discovery.write(discoveryBytes) == discoveryBytes.size(),
        "create remote capability fixture");
    discovery.close();
    require(QFile::setPermissions(discoveryPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner),
        "remote capability is owner-only");
    const auto discover = [&](const QString& path) {
        QProcess process;
        process.start(QStringLiteral("sh"),
            { QStringLiteral(TEST_SOURCE_DIR "/src/automation/RemoteDiscovery.sh"), QStringLiteral("descriptors"),
                QString(), path });
        require(process.waitForFinished(2000) && process.exitCode() == 0,
            "shared remote discovery is bounded and completes");
        return process.readAllStandardOutput().trimmed();
    };
    require(discover(discoveryPath) == discoveryBytes, "shared discovery reads the explicit private remote capability");
    require(
        QFile::setPermissions(discoveryPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadOther),
        "make remote capability publicly readable");
    require(discover(discoveryPath).isEmpty(), "remote discovery rejects publicly readable capabilities");
    require(QFile::setPermissions(discoveryPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner),
        "restore private remote capability");
    const QString remoteLink = temporary.filePath(QStringLiteral("remote-link.json"));
    require(::symlink(QFile::encodeName(discoveryPath).constData(), QFile::encodeName(remoteLink).constData()) == 0,
        "create remote descriptor symlink");
    require(discover(remoteLink).isEmpty(), "remote discovery rejects descriptor symlinks");
    if (::geteuid() == 0) {
        const QString sandbox = temporary.filePath(QStringLiteral("app-sandbox"));
        require(QDir().mkdir(sandbox)
                && QFile::setPermissions(
                    sandbox, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner),
            "create private app-owned descriptor directory");
        const QString sandboxDescriptor = QDir(sandbox).filePath(QStringLiteral("app.json"));
        QFile sandboxFile(sandboxDescriptor);
        require(sandboxFile.open(QIODevice::WriteOnly) && sandboxFile.write(discoveryBytes) == discoveryBytes.size(),
            "create app sandbox capability");
        sandboxFile.close();
        require(QFile::setPermissions(sandboxDescriptor, QFileDevice::ReadOwner | QFileDevice::WriteOwner)
                && ::chown(QFile::encodeName(sandboxDescriptor).constData(), 65534, 65534) == 0
                && ::chown(QFile::encodeName(sandbox).constData(), 65534, 65534) == 0,
            "stage the real root-to-app-UID ownership boundary");
        require(discover(sandboxDescriptor) == discoveryBytes,
            "root can discover an app-owned private capability without changing ownership");
        require(::chown(QFile::encodeName(sandboxDescriptor).constData(), 65533, 65533) == 0,
            "stage mismatched descriptor and registry owners");
        require(discover(sandboxDescriptor).isEmpty(),
            "root discovery still rejects descriptor and registry owner mismatch");
        require(::chown(QFile::encodeName(sandboxDescriptor).constData(), 0, 0) == 0
                && ::chown(QFile::encodeName(sandbox).constData(), 0, 0) == 0,
            "restore test-created sandbox ownership");
    } else {
        std::cout << "SKIP subcase: root-to-app-UID discovery requires uid0\n";
    }
    // Fake only the external SSH peer; production owns framing and child lifecycle.
    const QString sshPath = temporary.filePath(QStringLiteral("ssh"));
    QFile ssh(sshPath);
    require(ssh.open(QIODevice::WriteOnly), "create external SSH transport fixture");
    const QByteArray fixture = R"PY(#!/usr/bin/env python3
import json, os, select, sys, time
token = "b" * 64
assert all(token not in argument for argument in sys.argv)
mode = os.environ.get("SPOOL_TEST_SSH_MODE", "")
if mode == "silent":
    with open(os.environ["SPOOL_TEST_SSH_PID"], "w") as file:
        file.write(str(os.getpid()))
    time.sleep(20)
elif mode == "failure":
    sys.stderr.write(token)
    sys.exit(1)
elif "-W" not in sys.argv:
    sys.stdin.buffer.read()
    print(json.dumps(dict(instance="remote-test", transport="tcp", endpoint="127.0.0.1", port=12345, token=token)))
else:
    assert sys.argv[sys.argv.index("-W") + 1] == "127.0.0.1:12345"
    request = json.loads(sys.stdin.buffer.readline())
    assert request["token"] == token
    if mode == "async":
        # Like an asynchronous server operation, cancellation follows peer EOF.
        readable, _, _ = select.select([sys.stdin.buffer], [], [], 0.15)
        if readable and sys.stdin.buffer.read(1) == b"":
            sys.exit(2)
    print(json.dumps(dict(id="wrong" if mode == "wrong-id" else request["id"], ok=True,
                          result=dict(command=request["command"], args=request["args"], authenticated=True))))
)PY";
    require(ssh.write(fixture) == fixture.size(), "write complete transport fixture");
    ssh.close();
    require(QFile::setPermissions(sshPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner),
        "external SSH fixture is executable");
    const QByteArray previousPath = qgetenv("PATH");
    qputenv("PATH", temporary.path().toUtf8() + ':' + previousPath);
    QJsonObject remoteDescriptor { { "instance", "remote-test" }, { "transport", "tcp" }, { "endpoint", "127.0.0.1" },
        { "port", 12345 }, { "token", QString(64, QLatin1Char('b')) } };
    const auto remote = [&](int timeoutMs = 2000) {
        return Spool::LocalControl::requestSsh(
            QStringLiteral("root@device.test"), remoteDescriptor, QStringLiteral("status"), {}, timeoutMs);
    };
    require(remote()["result"].toObject()["authenticated"].toBool(),
        "SSH stdio framing authenticates using stdin, never argv");
    qputenv("SPOOL_TEST_SSH_MODE", "async");
    require(remote()["result"].toObject()["authenticated"].toBool(),
        "forwarded request remains open until the asynchronous reply is complete");
    qputenv("SPOOL_TEST_SSH_MODE", "wrong-id");
    require(code(remote()) == QStringLiteral("invalid_response"), "SSH replies require matching request identity");
    qputenv("SPOOL_TEST_SSH_MODE", "failure");
    const auto failed = remote();
    require(
        code(failed) == QStringLiteral("ssh_failed") && !QJsonDocument(failed).toJson().contains(QByteArray(64, 'b')),
        "SSH stderr containing a capability is not exposed in errors");
    qunsetenv("SPOOL_TEST_SSH_MODE");
    for (const QString& host : { QStringLiteral("-oProxyCommand=bad"), QStringLiteral("root@host;bad"),
             QStringLiteral("host\nbad"), QStringLiteral("root@host name") }) {
        require(code(Spool::LocalControl::requestSsh(host, remoteDescriptor, QStringLiteral("status"), {}, 100))
                == QStringLiteral("ssh_failed"),
            "SSH destination cannot inject shell syntax or options");
    }
    const QString pidFile = temporary.filePath(QStringLiteral("ssh.pid"));
    qputenv("SPOOL_TEST_SSH_PID", pidFile.toUtf8());
    qputenv("SPOOL_TEST_SSH_MODE", "silent");
    QElapsedTimer sshDeadline;
    sshDeadline.start();
    require(code(remote(1000)) == QStringLiteral("ssh_failed") && sshDeadline.elapsed() < 2500,
        "silent SSH transport has a bounded deadline");
    QFile pidInput(pidFile);
    require(pidInput.open(QIODevice::ReadOnly), "timed-out SSH peer started");
    const int childPid = pidInput.readAll().trimmed().toInt();
    require(childPid > 0 && ::kill(childPid, 0) == -1 && errno == ESRCH, "timed-out SSH child is killed and reaped");
    qunsetenv("SPOOL_TEST_SSH_MODE");
    qunsetenv("SPOOL_TEST_SSH_PID");
    const auto cli = [&](QStringList arguments) {
        QProcess process;
        process.start(QStringLiteral(SPOOL_TEST_SPOOLET), arguments);
        require(process.waitForFinished(5000), "remote CLI completes within its deadline");
        const QByteArray output = process.readAllStandardOutput();
        require(!output.contains(QByteArray(64, 'b')) && process.readAllStandardError().isEmpty(),
            "remote CLI never prints the capability or SSH stderr");
        return QJsonDocument::fromJson(output).object();
    };
    const QStringList target { QStringLiteral("--host"), QStringLiteral("root@device.test"),
        QStringLiteral("--instance"), QStringLiteral("remote-test") };
    const auto measured = cli(target + QStringList { QStringLiteral("speed-test") });
    require(measured["ok"].toBool() && measured["result"].toObject()["command"] == "speed-test"
            && measured["result"].toObject()["args"].toObject()["refresh"].toBool(),
        "remote speed-test starts measurement through the real CLI");
    const auto status = cli(target + QStringList { QStringLiteral("speed-test"), QStringLiteral("status") });
    require(status["ok"].toBool() && !status["result"].toObject()["args"].toObject()["refresh"].toBool(),
        "remote speed-test status does not restart measurement");
    const auto sorted = cli(target + QStringList { "browse", "sort", "Bitrate", "Descending" });
    require(sorted["ok"].toBool() && sorted["result"].toObject()["command"] == "browse-sort"
            && sorted["result"].toObject()["args"].toObject()["sortBy"] == "Bitrate"
            && sorted["result"].toObject()["args"].toObject()["sortOrder"] == "Descending",
        "CLI forwards server-side sort and direction without loading library pages");
    for (const QStringList& invalidSort : { QStringList { "browse", "sort", "Bitrate", "backwards" },
             QStringList { "browse", "sort", "Bitrate&token=secret" }, QStringList { "browse", "sort" } })
        require(!cli(target + invalidSort)["ok"].toBool(), "CLI rejects malformed server sort arguments");
    require(!cli({ "--host", "root@device.test", "--descriptor", "relative.json", "status" })["ok"].toBool(),
        "remote explicit descriptor must be an absolute remote path");
    require(!cli({ "--host", "root@device.test", "--instance", "remote-test", "--descriptor", "/private/test.json",
                "status" })["ok"]
                .toBool(),
        "instance and explicit remote descriptor remain mutually exclusive");
    qputenv("PATH", previousPath);
#endif
    return 0;
}
