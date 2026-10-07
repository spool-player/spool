#include "TestMain.h"
#include "TestRequire.h"
#include "automation/LocalCommandServer.h"
#include "automation/LocalControlProtocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QTemporaryDir>
#include <QThread>
#include <chrono>
#include <future>
#include <iostream>
#ifdef Q_OS_UNIX
#include <cstring>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using SpoolTests::require;

namespace {
Spool::LocalCommandServer server()
{
    // Startup/admission does not consume application state. Authentication must
    // reject before command dispatch can reach any application dependency.
    return Spool::LocalCommandServer(nullptr, nullptr, nullptr, nullptr, nullptr);
}
#ifdef Q_OS_UNIX
void leaveSocket(const QString& endpoint)
{
    const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
    require(descriptor >= 0, "create real native socket");
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const auto bytes = QFile::encodeName(endpoint);
    require(bytes.size() < qsizetype(sizeof(address.sun_path)), "test socket path fits native address");
    std::memcpy(address.sun_path, bytes.constData(), size_t(bytes.size()) + 1);
    require(::bind(descriptor, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0,
        "bind real socket inode");
    require(::close(descriptor) == 0, "leave unreachable owned socket inode");
}
#endif
}

SPOOL_TEST_MAIN("spoolet-admission")
{
    QCoreApplication app(argc, argv);
    QTemporaryDir data;
    require(data.isValid(), "private admission data root");
    qputenv("SPOOL_DATA_HOME", data.path().toUtf8());
#ifdef Q_OS_UNIX
    QTemporaryDir runtime(QStringLiteral("/tmp/spool-admission-XXXXXX"));
    require(runtime.isValid(), "private bounded admission runtime");
    qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
#endif
    QString error;
    const QString registry = Spool::LocalControl::directory(&error);
    require(!registry.isEmpty(), "private descriptor registry");
    const QString endpoint = Spool::LocalControl::localEndpoint(registry, QStringLiteral("admission"), &error);
    require(!endpoint.isEmpty(), "bounded reserved endpoint");
#ifndef Q_OS_UNIX
    QString firstPipe;
    {
        auto local = server();
        require(local.start(QStringLiteral("nonce"), &error), "create nonce-scoped named pipe");
        firstPipe = Spool::LocalControl::readDescriptor(QDir(registry).filePath("nonce.json"))["endpoint"].toString();
        require(!firstPipe.isEmpty(), "actual named pipe descriptor is accepted");
        local.stop();
    }
    {
        auto local = server();
        require(local.start(QStringLiteral("nonce"), &error), "recreate same namespace named pipe");
        const auto next = Spool::LocalControl::readDescriptor(QDir(registry).filePath("nonce.json"))["endpoint"].toString();
        require(!next.isEmpty() && next != firstPipe, "same instance retains a fresh unpredictable pipe nonce");
        local.stop();
    }
#endif
#ifdef Q_OS_UNIX
    {
        QFile marker(endpoint);
        require(marker.open(QIODevice::WriteOnly) && marker.write("preserve") == 8, "reserve unrelated regular file");
        marker.close();
        auto control = server();
        require(!control.start(QStringLiteral("admission"), &error), "regular file cannot be removed for admission");
        require(marker.open(QIODevice::ReadOnly) && marker.readAll() == "preserve", "regular file contents preserved");
        marker.close();
        require(marker.remove(), "remove owned test marker");
    }
    const QString target = data.filePath(QStringLiteral("symlink-target"));
    {
        QFile marker(target);
        require(marker.open(QIODevice::WriteOnly) && marker.write("preserve") == 8, "create owned symlink target");
    }
    require(::symlink(QFile::encodeName(target).constData(), QFile::encodeName(endpoint).constData()) == 0,
        "create real endpoint symlink");
    {
        auto control = server();
        require(!control.start(QStringLiteral("admission"), &error), "symlink cannot be admitted or removed");
        require(QFileInfo(endpoint).isSymLink() && QFileInfo::exists(target), "symlink and target are preserved");
    }
    require(QFile::remove(endpoint), "remove owned test symlink");
    {
        QLocalServer live;
        live.setSocketOptions(QLocalServer::UserAccessOption);
        require(live.listen(endpoint), "real live endpoint");
        auto control = server();
        require(!control.start(QStringLiteral("admission"), &error), "live socket cannot be replaced");
        require(live.isListening() && QFileInfo::exists(endpoint), "live socket remains intact");
    }
    if (::geteuid() != 0) {
        QLocalServer live;
        live.setSocketOptions(QLocalServer::UserAccessOption);
        require(live.listen(endpoint), "real permission-denied live endpoint");
        require(::chmod(QFile::encodeName(endpoint).constData(), 0000) == 0, "deny connections without closing listener");
        auto control = server();
        require(!control.start(QStringLiteral("admission"), &error),
            "an inconclusive access-denied probe cannot authorize socket removal");
        require(live.isListening() && QFileInfo::exists(endpoint), "unconnectable live socket remains intact");
        require(::chmod(QFile::encodeName(endpoint).constData(), 0600) == 0, "restore owned test socket permissions");
    }
    leaveSocket(endpoint);
    {
        auto control = server();
        require(control.start(QStringLiteral("admission"), &error), "owned unreachable socket is recovered");
        const auto descriptor = Spool::LocalControl::readDescriptor(QDir(registry).filePath("admission.json"));
        require(descriptor["endpoint"] == endpoint, "recovered server publishes its genuine reserved endpoint");
        control.stop();
    }
    const bool requireForeign = app.arguments().contains(QStringLiteral("--require-foreign-owner"));
    if (::geteuid() == 0) {
        leaveSocket(endpoint);
        require(::chown(QFile::encodeName(endpoint).constData(), 65534, 65534) == 0, "stage real foreign-owned inode");
        auto control = server();
        require(!control.start(QStringLiteral("admission"), &error), "foreign-owned socket is not removed");
        struct stat preserved {};
        require(::lstat(QFile::encodeName(endpoint).constData(), &preserved) == 0 && preserved.st_uid == 65534
                && S_ISSOCK(preserved.st_mode), "foreign-owned socket is preserved");
        require(::unlink(QFile::encodeName(endpoint).constData()) == 0, "remove test-created foreign-owned socket");
    } else {
        require(!requireForeign, "foreign-owner CI contract requires a privileged test process");
        std::cout << "SKIP subcase: foreign-owner admission requires uid0; privileged CI exercises it\n";
    }
    const QString longRuntime = data.filePath(QString(80, QLatin1Char('x')) + '/' + QString(80, QLatin1Char('y')));
    require(QDir().mkpath(longRuntime) && QFile::setPermissions(longRuntime,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner), "private oversized runtime");
    qputenv("XDG_RUNTIME_DIR", longRuntime.toUtf8());
    require(Spool::LocalControl::localEndpoint(registry, QStringLiteral("admission"), &error).isEmpty()
            && !error.isEmpty(), "native socket limit is rejected before listen");
    qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
#endif
    // Exercise the actual authenticated TCP server, not an echo implementation.
    auto tcp = server();
    require(tcp.startTcp(QStringLiteral("tcp-admission"), 0, &error), "explicit loopback TCP admission");
    auto descriptor = Spool::LocalControl::readDescriptor(QDir(registry).filePath("tcp-admission.json"));
    require(descriptor["transport"] == "tcp" && descriptor["endpoint"] == "127.0.0.1"
            && descriptor["port"].toInt() > 0, "private descriptor reflects actual loopback ephemeral listener");
    descriptor.insert("token", QString(64, QLatin1Char('0')));
    auto denied = std::async(std::launch::async, [descriptor] {
        return Spool::LocalControl::request(descriptor, QStringLiteral("text"), { { "text", "never-dispatch" } }, 1000);
    });
    QElapsedTimer deadline;
    deadline.start();
    while (denied.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready && deadline.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    require(denied.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, "TCP denial is bounded");
    require(denied.get()["error"].toObject()["code"] == "unauthorized", "actual TCP authenticates before text dispatch");
    tcp.stop();
    require(!QFileInfo::exists(QDir(registry).filePath("tcp-admission.json")), "TCP descriptor is removed on clean shutdown");
    return 0;
}
