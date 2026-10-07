#include "TestMain.h"
#include "provider/LanProbe.h"
#include "provider/ScriptRuntime.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

template <typename T> void rejects(QCoro::Task<T> task, const char *code)
{
    try {
        QCoro::waitFor(std::move(task));
    } catch (const std::exception& error) {
        require(QByteArray(error.what()) == code, "operation returns the expected stable error");
        return;
    }
    require(false, "operation should reject");
}
} // namespace

SPOOL_TEST_MAIN("script-runtime-network")
{
    QCoreApplication app(argc, argv);
    using namespace Spool;
    const auto up = QNetworkInterface::IsUp | QNetworkInterface::IsRunning;
    const auto targets = lanProbeTargets({ { "docker0", up, QHostAddress("172.17.0.1"), 16 },
        { "tun0", up | QNetworkInterface::IsPointToPoint, QHostAddress("10.1.1.2"), 24 },
        { "eth0", up, QHostAddress("192.168.2.51"), 24 }, { "wlan0", up, QHostAddress("169.254.3.4"), 16 },
        { "public", up, QHostAddress("8.8.8.8"), 24 }, { "ipv6", up, QHostAddress("fd00::1"), 64 } });
    require(targets.size() == 509 && targets.front() == QHostAddress::LocalHost,
        "two preferred private networks each have at most 254 targets plus local host");
    require(targets.contains(QHostAddress("192.168.2.51")) && targets.contains(QHostAddress("169.254.3.4")),
        "own addresses remain discoverable");
    for (const auto& address : targets)
        require(address == QHostAddress::LocalHost || address.toString().startsWith("192.168.2.")
                || address.toString().startsWith("169.254.3."),
            "physical LAN ranks before tunnels and containers");
    require(lanProbeTargets({}).isEmpty(), "no interfaces means no localhost probe");
    require(
        lanProbeTargets({ { "public", up, QHostAddress("8.8.8.8"), 24 }, { "ipv6", up, QHostAddress("fd00::1"), 64 } })
            .isEmpty(),
        "public and IPv6 addresses never become targets");
    require(lanProbeTargets({ { "small", up, QHostAddress("192.168.1.5"), 30 } })
            == QList<QHostAddress> { QHostAddress::LocalHost, QHostAddress("192.168.1.5"),
                QHostAddress("192.168.1.6") },
        "small subnet probing does not escape its configured prefix");

    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "loopback server starts");
    const QString origin = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    int requests = 0;
    int dripActive = 0;
    int dripMaximum = 0;
    std::function<void()> onDrip;
    QObject::connect(&server, &QTcpServer::newConnection, &app, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::disconnected, &app, [&, socket] {
                if (socket->property("drip").toBool())
                    --dripActive;
            });
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n") || socket->property("answered").toBool())
                    return;
                socket->setProperty("answered", true);
                ++requests;
                require(!request.contains("Cookie:") && !request.contains("Authorization:"),
                    "probe never sends credentials or cached cookies");
                if (request.startsWith("GET /drip ")) {
                    socket->setProperty("drip", true);
                    dripMaximum = std::max(dripMaximum, ++dripActive);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: 10000\r\n\r\nx");
                    auto *timer = new QTimer(socket);
                    QObject::connect(timer, &QTimer::timeout, socket, [socket] { socket->write("x"); });
                    timer->start(40);
                    if (onDrip)
                        onDrip();
                    return;
                }
                QByteArray body = "public-info";
                QByteArray extra
                    = "ETag: fixture-etag\r\nX-Plex-Client-Identifier: companion\r\nSet-Cookie: secret=1\r\n";
                QByteArray status = "200 OK";
                if (request.startsWith("GET /oversized "))
                    body = QByteArray(4097, 'x');
                if (request.startsWith("GET /redirect ")) {
                    status = "302 Found";
                    extra += "Location: /must-not-follow\r\n";
                }
                if (request.startsWith("GET /headers-large ")) {
                    extra.clear();
                    for (int i = 0; i < 16; ++i)
                        extra += "X-Field-" + QByteArray::number(i) + ": " + QByteArray(4200, 'x') + "\r\n";
                }
                socket->write("HTTP/1.1 " + status + "\r\n" + extra
                    + "Connection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        }
    });

    int snapshots = 0;
    ScriptRuntime::NetworkHooks hooks;
    hooks.lanTargets = [&snapshots] {
        ++snapshots;
        return QList<QHostAddress>(40, QHostAddress(QHostAddress::LocalHost));
    };
    ScriptRuntime runtime(QStringLiteral(TEST_SOURCE_DIR "/tests/providers/fixtures/network.mjs"), {}, hooks);
    const QVariantMap capabilities { { "httpMetadata", true }, { "originGrants", true }, { "lanProbe", true },
        { "discovery", true } };
    QCoro::waitFor(runtime.addSource("draft", {}, { QUrl(origin) }, capabilities, true));
    QCoro::waitFor(runtime.addSource("other", {}, {}, capabilities, true));
    QCoro::waitFor(runtime.addSource("account", {}, {}, capabilities));
    QCoro::waitFor(runtime.addSource("baseline", {}, { QUrl(origin) }));
    rejects(runtime.call("baseline", "discover", { { "port", 7359 }, { "timeout", 100 } }), "unsupported_capability");
    const auto udp = QCoro::waitFor(runtime.call(
        "draft", "discover", { { "port", 7359 }, { "message", "spool-discovery-contract-test" }, { "timeout", 100 } }));
    require(
        udp.value("array").toBool(), "UDP discovery publishes an ordinary JavaScript array usable by provider code");
    const auto http = [&](QString source, QString path, QVariantMap options = {}) {
        return runtime.call(source, "http", { { "url", origin + path }, { "options", options } });
    };
    const QVariantMap metadata { { "responseHeaders",
        QVariantList { "ETag", "X-PLEX-Client-Identifier", "missing" } } };
    require(!QCoro::waitFor(http("draft", "/")).contains("headers"), "ordinary responses have no extra headers map");
    const auto headers = QCoro::waitFor(http("draft", "/", metadata)).value("headers").toMap();
    require(headers == QVariantMap { { "etag", "fixture-etag" }, { "x-plex-client-identifier", "companion" } },
        "only requested present headers are returned with lowercase names");
    const int beforeInvalid = requests;
    rejects(http("baseline", "/", metadata), "unsupported_capability");
    rejects(http("draft", "/", { { "responseHeaders", QVariantList { "Set-Cookie" } } }), "header_denied");
    rejects(http("draft", "/", { { "responseHeaders", QVariantList { "Set-Cookie2" } } }), "header_denied");
    rejects(http("draft", "/", { { "responseHeaders", QVariantList { "bad\r\nname" } } }), "header_denied");
    rejects(http("draft", "/", { { "responseHeaders", QVariantList { "etag\n" } } }), "header_denied");
    rejects(http("draft", "/", { { "responseHeaders", QVariantList(17, "etag") } }), "header_denied");
    require(requests == beforeInvalid, "invalid header options reject before network work");
    QVariantList largeHeaders;
    for (int i = 0; i < 16; ++i)
        largeHeaders.append(QStringLiteral("x-field-%1").arg(i));
    rejects(http("draft", "/headers-large", { { "responseHeaders", largeHeaders } }), "response_limit");

    rejects(http("account", "/"), "request_denied");
    rejects(runtime.grantOrigins("account", { QUrl(origin + "/path") }), "origin_denied");
    rejects(runtime.grantOrigins("account", { QUrl("http://user@127.0.0.1") }), "origin_denied");
    rejects(runtime.grantOrigins("account", { QUrl("*") }), "origin_denied");
    rejects(runtime.grantOrigins("account", { QUrl("ws://127.0.0.1") }), "origin_denied");
    rejects(runtime.grantOrigins("baseline", { QUrl(origin) }), "unsupported_capability");
    QCoro::waitFor(runtime.grantOrigins("account", { QUrl(origin) }));
    require(QCoro::waitFor(http("account", "/")).value("status").toInt() == 200,
        "approved origin reaches current operation host");
    require(QCoro::waitFor(runtime.call("account", "sourceHttp", { { "url", origin } })).value("status").toInt() == 200,
        "approved origin reaches the already-created source host");
    require(QCoro::waitFor(runtime.call("account", "state")).value("calls").toInt() == 2,
        "grant does not recreate source-private state");
    rejects(http("other", "/"), "request_denied");

    QCoro::waitFor(runtime.addSource("staged", {}, {}, capabilities));
    auto approval = std::make_shared<std::atomic_bool>(false);
    QCoro::waitFor(runtime.grantOrigins("staged", { QUrl(origin) }, approval));
    rejects(http("staged", "/"), "request_denied");
    approval.reset();
    rejects(http("staged", "/"), "request_denied");
    approval = std::make_shared<std::atomic_bool>(false);
    QCoro::waitFor(runtime.grantOrigins("staged", { QUrl(origin) }, approval));
    rejects(http("staged", "/"), "request_denied");
    approval->store(true);
    require(QCoro::waitFor(http("staged", "/")).value("status").toInt() == 200,
        "staged origin becomes usable only after the main-thread authorization barrier");
    approval.reset();
    require(QCoro::waitFor(http("staged", "/")).value("status").toInt() == 200,
        "committed origin remains approved after the consent transaction ends");

    QVariantMap probe { { "port", server.serverPort() }, { "path", "/" }, { "limit", 32 } };
    rejects(runtime.call("draft", "probe", probe), "discovery_denied");
    rejects(runtime.allowLanDiscovery("account"), "unsupported_capability");
    rejects(runtime.allowLanDiscovery("baseline"), "unsupported_capability");
    QCoro::waitFor(runtime.allowLanDiscovery("draft"));
    QCoro::waitFor(runtime.allowLanDiscovery("other"));
    require(QCoro::waitFor(runtime.call("draft", "state")).value("sourceProbe").toString() == "undefined",
        "long-lived source host cannot invoke local HTTP discovery");
    const int beforePage = requests;
    const auto page = QCoro::waitFor(runtime.call("draft", "probe", probe));
    require(page.value("responses").toList().size() == 32 && requests - beforePage == 32 && snapshots == 1,
        "a discovery page probes at most 32 snapshot targets");
    const QString cursor = page.value("cursor").toString();
    require(!cursor.isEmpty() && !page.value("exhausted").toBool(), "remaining targets have an opaque continuation");
    probe["cursor"] = cursor;
    rejects(runtime.call("other", "probe", probe), "invalid_cursor");
    auto second = QCoro::waitFor(runtime.call("draft", "probe", probe));
    require(second.value("responses").toList().size() == 8 && second.value("exhausted").toBool()
            && second.value("cursor").isNull() && snapshots == 1,
        "continuation consumes the same snapshot exactly once");
    rejects(runtime.call("draft", "probe", probe), "invalid_cursor");
    probe.remove("cursor");
    probe["limit"] = 1;
    probe["path"] = "/oversized";
    require(QCoro::waitFor(runtime.call("draft", "probe", probe)).value("responses").toList().isEmpty(),
        "oversized public responses are discarded, not truncated into valid JSON");
    probe["path"] = "/redirect";
    const int beforeRedirect = requests;
    require(QCoro::waitFor(runtime.call("draft", "probe", probe))
                    .value("responses")
                    .toList()
                    .front()
                    .toMap()
                    .value("status")
                    .toInt()
                == 302
            && requests == beforeRedirect + 1,
        "probe reports redirects without following them");
    for (const QString& invalid :
        { QStringLiteral("//example.com/"), QStringLiteral("/?key=secret"), QStringLiteral("/#fragment") }) {
        probe["path"] = invalid;
        rejects(runtime.call("draft", "probe", probe), "discovery_denied");
    }
    probe["path"] = "/drip";
    probe["limit"] = 32;
    QElapsedTimer elapsed;
    elapsed.start();
    const auto drip = QCoro::waitFor(runtime.call("draft", "probe", probe));
    require(elapsed.elapsed() < 1000 && drip.value("responses").toList().isEmpty() && !drip.value("exhausted").toBool(),
        "wall deadline aborts incomplete drip feeds and retains unstarted targets for continuation");
    require(dripMaximum == 4, "at most four local HTTP requests run concurrently");
    const auto drained = [&] {
        QEventLoop loop;
        QTimer::singleShot(100, &loop, &QEventLoop::quit);
        loop.exec();
        require(dripActive == 0, "deadline and cancellation close every incomplete reply");
    };
    drained();
    const QString stale = drip.value("cursor").toString();
    runtime.cancelLanDiscovery("draft");
    rejects(runtime.call("draft", "probe", probe), "discovery_denied");
    QCoro::waitFor(runtime.allowLanDiscovery("draft"));
    probe["cursor"] = stale;
    rejects(runtime.call("draft", "probe", probe), "invalid_cursor");
    probe.remove("cursor");
    onDrip = [&] { runtime.cancelScope("draft", "lan"); };
    rejects(runtime.call("draft", "probe", probe, "lan"), "action_cancelled");
    onDrip = {};
    drained();
    onDrip = [&] { runtime.cancelLanDiscovery("draft"); };
    rejects(runtime.call("draft", "probe", probe), "action_cancelled");
    onDrip = {};
    drained();
    onDrip = [&] { runtime.removeSource("other"); };
    rejects(runtime.call("other", "probe", probe), "source_removed");
    onDrip = {};
    drained();
    // A worker round-trip observes completed cancellation; no real interface enumeration occurred.
    QCoro::waitFor(runtime.call("draft", "state"));
    runtime.cancelLanDiscovery("draft");
    rejects(runtime.call("draft", "probe", probe), "discovery_denied");
    ScriptRuntime::NetworkHooks emptyHooks;
    emptyHooks.lanTargets = [] { return QList<QHostAddress>(); };
    ScriptRuntime empty(QStringLiteral(TEST_SOURCE_DIR "/tests/providers/fixtures/network.mjs"), {}, emptyHooks);
    QCoro::waitFor(empty.addSource("draft", {}, {}, capabilities, true));
    QCoro::waitFor(empty.allowLanDiscovery("draft"));
    const auto noInterfaces = QCoro::waitFor(empty.call("draft", "probe", probe));
    require(noInterfaces.value("exhausted").toBool() && noInterfaces.value("responses").toList().isEmpty(),
        "an empty target snapshot finishes without issuing a request");
    return 0;
}
