#include "TestMain.h"
#include "provider/LanProbe.h"
#include "provider/ScriptRuntime.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QSet>
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

void plexSessions()
{
    using namespace Spool;
    QTcpServer cloud, first, second, backup, alternate1, alternate2, alternate3, unapproved;
    for (QTcpServer *server : { &cloud, &first, &second, &backup, &alternate1, &alternate2, &alternate3, &unapproved })
        require(server->listen(QHostAddress::LocalHost), "Plex protocol fixture listens on loopback");
    const auto origin
        = [](const QTcpServer& server) { return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()); };
    const QString cloudOrigin = origin(cloud), firstOrigin = origin(first);
    const QString secondOrigin = origin(second), backupOrigin = origin(backup);
    const QString unapprovedOrigin = origin(unapproved);
    bool denyGuestSwitch = false, expireGuest = false, removeFirst = false, hangFirst = false;
    int linkPolls = 0, guestSwitches = 0, deniedOriginRequests = 0;
    QSet<quint16> probedPorts;
    QObject::connect(&unapproved, &QTcpServer::newConnection, &unapproved, [&] {
        ++deniedOriginRequests;
        while (QTcpSocket *socket = unapproved.nextPendingConnection())
            socket->deleteLater();
    });
    std::function<void()> onHome, onProbe;
    const auto serve = [&](QTcpServer& server, const QString& identity) {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&, identity, owner = &server] {
            while (QTcpSocket *socket = owner->nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, identity] {
                    const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    const qsizetype headerEnd = request.indexOf("\r\n\r\n");
                    if (headerEnd < 0 || socket->property("answered").toBool())
                        return;
                    int bodySize = 0;
                    QByteArray token;
                    for (QByteArray line : request.left(headerEnd).split('\n')) {
                        line = line.trimmed();
                        if (line.toLower().startsWith("content-length:"))
                            bodySize = line.mid(15).trimmed().toInt();
                        if (line.toLower().startsWith("x-plex-token:"))
                            token = line.mid(13).trimmed();
                    }
                    if (request.size() < headerEnd + 4 + bodySize)
                        return;
                    socket->setProperty("answered", true);
                    const QByteArray path = request.split(' ').value(1).split('?').front();
                    const QByteArray form = request.mid(headerEnd + 4, bodySize);
                    QByteArray body;
                    int status = 200;
                    if (identity != "cloud") {
                        probedPorts.insert(socket->localPort());
                        require(path == "/" && token.endsWith(identity == "other" ? "-other" : "-pms"),
                            "PMS probes use only the selected resource credential");
                        if (onProbe)
                            onProbe();
                        if (hangFirst && socket->localPort() == first.serverPort())
                            return;
                        body = QJsonDocument(
                            QJsonObject { { "MediaContainer", QJsonObject { { "machineIdentifier", identity } } } })
                                   .toJson(QJsonDocument::Compact);
                    } else if (path == "/api/v2/pins/7") {
                        ++linkPolls;
                        require(token.isEmpty(), "device link poll is not authenticated with a PMS/member token");
                        body = R"({"authToken":"fixture-owner"})";
                    } else if (path == "/api/home/users") {
                        require(token == "fixture-owner", "Home roster uses retained linked credentials");
                        if (onHome)
                            onHome();
                        body
                            = R"(<MediaContainer><User id="1" title="Owner" protected="1" admin="1"/><User id="2" title="Child" protected="1" restricted="1"/><User id="3" title="Guest" protected="0" restricted="1"/></MediaContainer>)";
                    } else if (path.startsWith("/api/home/users/") && path.endsWith("/switch")) {
                        require(token == "fixture-owner" && request.startsWith("POST ")
                                && !request.left(headerEnd).contains("pin="),
                            "Home switching uses owner credential and a transient form body");
                        const QByteArray member = path.split('/').value(4);
                        if (member == "3")
                            ++guestSwitches;
                        if ((member != "3" && form != "pin=1234") || (member == "3" && denyGuestSwitch))
                            status = 401;
                        else
                            body = "<user id=\"" + member + "\" authenticationToken=\"fixture-"
                                + (member == "2"        ? "member"
                                        : member == "3" ? "guest"
                                                        : "owner")
                                + "\"/>";
                    } else if (path == "/api/v2/user") {
                        require(token == "fixture-owner" || token == "fixture-member" || token == "fixture-guest",
                            "plex.tv identity never receives a resource token");
                        if (expireGuest && token == "fixture-guest")
                            status = 401;
                        else
                            body = QJsonDocument(
                                QJsonObject { { "id",
                                                  token == "fixture-owner"        ? 1
                                                      : token == "fixture-member" ? 2
                                                                                  : 3 },
                                    { "title", "Fixture viewer" }, { "restricted", token != "fixture-owner" } })
                                       .toJson();
                    } else if (path == "/api/v2/resources") {
                        require(token == "fixture-owner" || token == "fixture-member" || token == "fixture-guest",
                            "accessible resources are requested under the selected member");
                        QJsonArray resources;
                        if (!removeFirst || token == "fixture-owner")
                            resources.append(QJsonObject { { "provides", "server" }, { "clientIdentifier", "machine" },
                                { "name", "First" }, { "accessToken", QString::fromLatin1(token) + "-pms" },
                                { "connections",
                                    QJsonArray { QJsonObject { { "uri", firstOrigin }, { "local", true } },
                                        QJsonObject { { "uri", backupOrigin } },
                                        QJsonObject { { "uri", origin(alternate1) } },
                                        QJsonObject { { "uri", origin(alternate2) } },
                                        QJsonObject { { "uri", origin(alternate3) } },
                                        QJsonObject { { "uri", unapprovedOrigin } } } } });
                        resources.append(QJsonObject { { "provides", "server" }, { "clientIdentifier", "other" },
                            { "name", "Second" }, { "accessToken", QString::fromLatin1(token) + "-other" },
                            { "connections", QJsonArray { QJsonObject { { "uri", secondOrigin } } } } });
                        body = QJsonDocument(resources).toJson();
                    } else {
                        ++deniedOriginRequests;
                        require(false, "Plex fixture received an unexpected protocol route");
                    }
                    socket->write("HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK" : " Unauthorized")
                        + "\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n"
                        + body);
                    socket->disconnectFromHost();
                });
            }
        });
    };
    serve(cloud, "cloud");
    serve(first, "machine");
    serve(second, "other");
    serve(backup, "machine");
    serve(alternate1, "machine");
    serve(alternate2, "machine");
    serve(alternate3, "machine");
    ScriptRuntime::NetworkHooks hooks;
    hooks.network = [](QNetworkAccessManager *manager) { manager->setProxy(QNetworkProxy::NoProxy); };
    ScriptRuntime plex(QStringLiteral(TEST_SOURCE_DIR "/tests/providers/fixtures/plex-network.mjs"), {}, hooks);
    const QVariantMap offers { { "accountActivation", true }, { "originGrants", true }, { "httpMetadata", true } };
    const QList<QUrl> approved { QUrl(cloudOrigin), QUrl(firstOrigin), QUrl(secondOrigin), QUrl(backupOrigin),
        QUrl(origin(alternate1)), QUrl(origin(alternate2)), QUrl(origin(alternate3)) };
    QHash<QString, QVariantMap> credentials;
    QObject::connect(&plex, &ScriptRuntime::event, &plex,
        [&](const QString& source, const QString& type, const QVariantMap& payload) {
            if (type == "configuration")
                credentials[source].insert(payload);
        });
    const auto add = [&](const QString& id, QVariantMap config, bool draft = false) {
        config.insert("fixtureEndpoint", cloudOrigin);
        QCoro::waitFor(plex.addSource(id, config, approved, offers, draft));
    };
    const auto call = [&](const QString& id, const QString& method, QVariantMap args = {}) {
        return QCoro::waitFor(plex.call(id, method, args));
    };
    const auto activation = [](QString reason, QVariantMap answers = {}) {
        QVariantMap value { { "reason", reason }, { "lastUsed", true } };
        if (!answers.isEmpty())
            value.insert("answers", answers);
        return value;
    };
    add("link", {}, true);
    const auto linked = call("link", "pinPoll", { { "id", "7" } });
    require(
        linked.value("homeUsers").toList().size() == 3 && !linked.value("user").toMap().contains("linkedAccountToken"),
        "actual provider lists Home without publishing linked credentials to QML");
    rejects(plex.call("link", "homeSelect", { { "userId", "2" }, { "pin", "wrong" } }), "invalid_pin");
    const auto chosen = call("link", "homeSelect", { { "userId", "2" }, { "pin", "1234" } });
    require(chosen.value("servers").toList().size() == 2
            && !chosen.value("servers").toList().front().toMap().contains("token"),
        "actual member resource chooser exposes no resource credential");
    const auto firstAccount = call("link", "connect", { { "serverId", "machine" } });
    require(firstAccount.value("account") == "2@machine" && !firstAccount.contains("configuration")
            && credentials.value("link").value("token") == "fixture-member-pms",
        "native credential event, not UI completion, retains the member resource credential");
    require(probedPorts.size() == 5 && deniedOriginRequests == 0,
        "bounded provider workers probe every approved candidate beyond the native four-request budget without "
        "expanding grants");
    const QVariantMap savedFirst = credentials.value("link");
    call("link", "connect", { { "serverId", "other" } });
    const QVariantMap savedSecond = credentials.value("link");
    require(savedFirst.value("linkedAccountToken") == "fixture-owner"
            && savedFirst.value("activeAccountToken") == "fixture-member"
            && savedSecond.value("token") == "fixture-member-other",
        "two servers retain one linked/member chain but distinct server credentials");
    plex.removeSource("link");
    add("restart", savedSecond);
    require(call("restart", "activate", activation("startup")).value("pick").toMap().value("kind") == "homePin",
        "protected restored member stays private until its PIN");
    rejects(plex.call("restart", "activate", activation("startup", { { "pin", "wrong" } })), "invalid_pin");
    call("restart", "activate", activation("startup", { { "pin", "1234" } }));
    require(credentials.value("restart").value("token") == "fixture-member-other",
        "protected bad PIN remains retryable and restores only its selected server credential");
    add("another", { { "setupAccount", savedFirst }, { "setupContext", QVariantMap { { "purpose", "addProfile" } } } },
        true);
    require(call("another", "setupResume").value("homeUsers").toList().size() == 3,
        "another profile uses retained owner link through native HTTP");
    call("another", "homeSelect", { { "userId", "3" }, { "pin", "" } });
    call("another", "connect", { { "serverId", "machine" } });
    const QVariantMap guest = credentials.value("another");
    require(linkPolls == 1 && guest.value("token") == "fixture-guest-pms"
            && guest.value("linkedAccountToken") == "fixture-owner",
        "multiple watching profiles use one device link without an owner PMS fallback");
    add("guest", guest);
    const int switched = guestSwitches;
    call("guest", "activate", activation("startup"));
    require(guestSwitches == switched, "unprotected restart reuses retained member credential");
    expireGuest = true;
    denyGuestSwitch = true;
    add("expired", guest);
    rejects(plex.call("expired", "activate", activation("switch")), "http_401");
    require(!credentials.contains("expired") && !call("expired", "describe").contains("artwork"),
        "unprotected expired session requests reauthentication without PIN loop or credential overwrite");
    expireGuest = false;
    denyGuestSwitch = false;
    removeFirst = true;
    add("permission", guest);
    rejects(plex.call("permission", "activate", activation("switch")), "permission_denied");
    require(!call("permission", "describe").contains("artwork") && !credentials.contains("permission"),
        "loss of member access fails closed even while owner/server credentials are retained");
    removeFirst = false;
    QVariantMap recover = savedFirst;
    recover["connections"]
        = QVariantList { QVariantMap { { "uri", firstOrigin } }, QVariantMap { { "uri", backupOrigin } } };
    add("stable", recover);
    call("stable", "activate", activation("linked"));
    require(credentials.value("stable").value("server") == firstOrigin,
        "server selection preserves preferred approved identity despite parallel replies");
    hangFirst = true;
    add("recover", recover);
    QElapsedTimer timer;
    timer.start();
    call("recover", "activate", activation("linked"));
    require(timer.elapsed() < 7000 && credentials.value("recover").value("server") == backupOrigin,
        "approved backup proves the same PMS identity within the bounded probe deadline");
    add("cancel", recover);
    onProbe = [&] { plex.cancelScope("cancel", "selection"); };
    rejects(plex.call("cancel", "activate", activation("linked"), "selection"), "action_cancelled");
    onProbe = {};
    require(!credentials.contains("cancel") && deniedOriginRequests == 0,
        "cancelled activation and newly advertised unapproved origin never commit or expand access");
    hangFirst = false;
    add("cancelHome", savedFirst);
    onHome = [&] { plex.cancelScope("cancelHome", "home"); };
    rejects(plex.call("cancelHome", "activate", activation("switch"), "home"), "action_cancelled");
    onHome = {};
    require(!credentials.contains("cancelHome"), "cancelled Home roster cannot resume retained credentials");
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
    plexSessions();
    return 0;
}
