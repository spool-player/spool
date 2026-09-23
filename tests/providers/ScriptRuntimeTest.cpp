#include "provider/ScriptRuntime.h"
#include "TestMain.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void rejects(QCoro::Task<QVariantMap> task, const char *message)
{
    bool failed = false;
    try {
        QCoro::waitFor(std::move(task));
    } catch (const std::exception& error) {
        failed = true;
        require(!QByteArray(error.what()).contains("private-token"), "errors must not disclose provider secrets");
    }
    require(failed, message);
}
}

SPOOL_TEST_MAIN("script-runtime")
{
    QCoreApplication app(argc, argv);
    using Spool::ScriptRuntime;
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "fixture server listens");
    const QString origin = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    QList<QByteArray> received;
    std::function<void()> cancelSlow;
    QObject::connect(&server, &QTcpServer::newConnection, &app, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &received, &cancelSlow] {
                QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n") || socket->property("answered").toBool())
                    return;
                socket->setProperty("answered", true);
                received.append(request);
                if (request.startsWith("GET /slow ")) {
                    cancelSlow();
                    return;
                }
                if (request.startsWith("GET /error ")) {
                    socket->write("HTTP/1.1 401 Unauthorized\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
                    socket->disconnectFromHost();
                    return;
                }
                if (request.startsWith("GET /truncated ")) {
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\n{}");
                    socket->disconnectFromHost();
                    return;
                }
                if (request.startsWith("GET /redirect ")) {
                    socket->write("HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/private\r\nContent-Length: "
                                  "0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                    return;
                }
                if (request.startsWith("GET /oversized ")) {
                    const QByteArray large(9 * 1024 * 1024, 'x');
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(large.size())
                        + "\r\nConnection: close\r\n\r\n");
                    socket->write(large);
                    socket->disconnectFromHost();
                    return;
                }
                const QByteArray body = R"({"items":[{"id":"same-id","name":"Example"}],"cursor":"next"})";
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nSet-Cookie: "
                              "session=private\r\nConnection: close\r\nContent-Length: "
                    + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        }
    });
    const QString entry = QStringLiteral(TEST_SOURCE_DIR "/tests/providers/fixtures/provider.mjs");
    auto runtime = std::make_unique<ScriptRuntime>(entry, QVariantMap {});
    const auto add = [&](const QString& id) {
        return runtime->addSource(
            id, { { "origin", origin }, { "label", id }, { "token", id + "-token" } }, { QUrl(origin) });
    };
    QCoro::waitFor(add("a"));
    QCoro::waitFor(add("b"));
    auto a = runtime->call("a", "page");
    auto b = runtime->call("b", "page");
    const auto pageA = QCoro::waitFor(std::move(a));
    const auto pageB = QCoro::waitFor(std::move(b));
    require(pageA.value("items").toList().front().toMap().value("title") == "a: Example", "source A owns its mapping");
    require(pageB.value("items").toList().front().toMap().value("title") == "b: Example", "source B owns its mapping");
    require(pageA.value("cursor") == "next", "opaque continuation survives conversion");
    require(pageA.value("calls").toInt() == 1 && pageB.value("calls").toInt() == 1,
        "source factories have independent state");
    require(received.size() == 2, "both concurrent sources request independently");
    require(received[0].contains("a-token") != received[1].contains("a-token"), "tokens do not cross source requests");
    QCoro::waitFor(runtime->call("b", "page"));
    require(!received.back().contains("Cookie:"), "shared transport does not inherit another source's cookies");
    require(QCoro::waitFor(runtime->call("a", "raw", { { "path", "error" } })).value("status").toInt() == 401,
        "HTTP errors remain provider-interpretable responses");
    require(QCoro::waitFor(runtime->call("a", "raw", { { "path", "redirect" } })).value("status").toInt() == 302,
        "redirects are not followed with source credentials");
    rejects(runtime->call("a", "raw", { { "path", "oversized" } }), "decompressed responses are bounded");
    rejects(runtime->call("a", "raw", { { "path", "truncated" } }), "truncated successful responses reject");
    require(QCoro::waitFor(runtime->call("a", "delay", { { "milliseconds", 10 } })).value("completed").toBool(),
        "native worker timers resume source Promise continuations");
    rejects(runtime->call("a", "delay", { { "milliseconds", -1 } }), "negative delay rejected");
    rejects(runtime->call("a", "delay", { { "milliseconds", 10001 } }), "timer duration is bounded");
    rejects(runtime->call("a", "delay", { { "milliseconds", 1.5 } }), "fractional timer values are not truncated");
    rejects(
        runtime->call("a", "delay", { { "milliseconds", 4294967296.0 } }), "timer values cannot wrap native integers");
    rejects(runtime->call("a", "denied", { { "url", "http://127.0.0.1:1/private" } }), "unauthorised origin rejected");
    rejects(runtime->call("a", "throws"), "synchronous exceptions settle operations");
    rejects(runtime->call("a", "cycle"), "cyclic results fail bounded conversion");
    rejects(runtime->call("a", "largeInteger"), "unsafe integer results rejected");
    rejects(runtime->call("a", "missing"), "missing feature reports unsupported operation");
    const auto code = [&](const char *method) {
        try {
            QCoro::waitFor(runtime->call("a", method));
        } catch (const std::exception& error) {
            return QByteArray(error.what());
        }
        return QByteArray();
    };
    require(code("expired") == "http_401", "a snake_case error code crosses to native code as is");
    require(code("throws") == "provider_error", "any other error text is replaced");

    QStringList events;
    QObject::connect(runtime.get(), &ScriptRuntime::event, &app,
        [&events](const QString& source, const QString& type, const QVariantMap& payload) {
            events.append(source + '/' + type + '/' + payload.value("itemId").toString());
        });
    QCoro::waitFor(runtime->call("b", "announce", { { "itemId", "x" } }));
    QElapsedTimer eventWait;
    eventWait.start();
    while (events.isEmpty() && eventWait.elapsed() < 2000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    require(events == QStringList { "b/changed/x" }, "host.emit from a source arrives tagged with that source");
    auto pendingTimer = runtime->call("a", "delay", { { "milliseconds", 10000 } });
    cancelSlow = [&] { runtime->removeSource("a"); };
    auto pending = runtime->call("a", "raw", { { "path", "slow" } });
    rejects(std::move(pending), "source removal cancels pending operations");
    rejects(std::move(pendingTimer), "source removal also cancels timer continuations");
    require(QCoro::waitFor(runtime->call("b", "state")).value("calls").toInt() == 2,
        "removal leaves other source operational");
    QCoro::waitFor(add("a"));
    require(QCoro::waitFor(runtime->call("a", "state")).value("calls").toInt() == 0,
        "replacement gets a fresh source generation");
    rejects(runtime->call("b", "never"), "operation deadline settles a Promise that never resolves");
    require(QCoro::waitFor(runtime->call("b", "state")).value("calls").toInt() == 2,
        "timing out one operation does not disable the source");
    auto shutdownPending = runtime->call("b", "never");
    runtime.reset();
    rejects(std::move(shutdownPending), "shutdown settles retained operations");

    for (const QString method : { QStringLiteral("spin"), QStringLiteral("asyncSpin"), QStringLiteral("timerSpin") }) {
        ScriptRuntime runaway(entry, QVariantMap {});
        QCoro::waitFor(runaway.addSource("a", {}, {}));
        QElapsedTimer elapsed;
        elapsed.start();
        int guiTurns = 0;
        QTimer responsiveness;
        QObject::connect(&responsiveness, &QTimer::timeout, &app, [&] { ++guiTurns; });
        responsiveness.start(10);
        rejects(runaway.call("a", method), "runaway JS is interrupted including Promise continuations");
        require(elapsed.elapsed() < 3000 && guiTurns >= 5, "watchdog bounds runaway work without blocking GUI events");
        rejects(runaway.call("a", "state"), "interrupted module is disabled rather than silently reused");
    }
    return 0;
}
