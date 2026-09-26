#include "provider/ScriptRuntime.h"
#include "TestMain.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
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
    QList<int> speedSizes;
    QSet<QString> speedNonces;
    QList<QTcpSocket *> failingRound;
    int speedActive = 0;
    int speedMaximum = 0;
    QObject::connect(&server, &QTcpServer::newConnection, &app, [&] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::disconnected, &app, [socket, &speedActive] {
                if (socket->property("speedActive").toBool())
                    --speedActive;
            });
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                if (!request.contains("\r\n\r\n") || socket->property("answered").toBool())
                    return;
                socket->setProperty("answered", true);
                received.append(request);
                const QUrl target(QString::fromLatin1(request.split(' ').value(1)));
                if (target.path().startsWith(QStringLiteral("/range"))) {
                    QByteArray range;
                    for (const QByteArray& header : request.split('\n')) {
                        if (header.toLower().startsWith("range: bytes="))
                            range = header.mid(13).trimmed();
                    }
                    const QList<QByteArray> bounds = range.split('-');
                    require(bounds.size() == 2, "media probe supplies a single byte range");
                    const int start = bounds[0].toInt();
                    const int end = bounds[1].toInt();
                    require(start >= 0 && end >= start && end < 4 * 1024 * 1024,
                        "media samples stay within the declared minimum resource size");
                    require(request.contains("Authorization: a-token"), "media ranges use account authentication");
                    const int bytes = end - start + 1;
                    QByteArray responseRange = "bytes " + range + "/8388608";
                    if (target.path() == QStringLiteral("/range-wrong"))
                        responseRange = "bytes 1-" + QByteArray::number(bytes) + "/8388608";
                    if (target.path() == QStringLiteral("/range-short-total"))
                        responseRange = "bytes " + range + '/' + QByteArray::number(end);
                    const QByteArray status
                        = target.path() == QStringLiteral("/range-ignored") ? "200 OK" : "206 Partial Content";
                    socket->write("HTTP/1.1 " + status + "\r\nContent-Range: " + responseRange
                        + "\r\nContent-Length: " + QByteArray::number(bytes) + "\r\nConnection: close\r\n\r\n");
                    socket->write(QByteArray(bytes, 'm'));
                    socket->disconnectFromHost();
                    return;
                }
                if (target.path().startsWith(QStringLiteral("/speed"))) {
                    const QUrlQuery query(target);
                    const int bytes = query.queryItemValue(QStringLiteral("bytes")).toInt();
                    require(bytes > 0 && bytes <= 4 * 1024 * 1024, "benchmark requests bounded samples");
                    speedSizes.append(bytes);
                    const QString nonce = query.queryItemValue(QStringLiteral("nonce"));
                    require(!nonce.isEmpty() && !speedNonces.contains(nonce), "every benchmark sample bypasses caches");
                    speedNonces.insert(nonce);
                    require(!request.contains("Cookie:"), "benchmark does not load shared cookies");
                    require(request.contains("Accept-Encoding: identity"), "benchmark requests uncompressed bytes");
                    socket->setProperty("speedActive", true);
                    speedMaximum = std::max(speedMaximum, ++speedActive);
                    if (target.path() == QStringLiteral("/speed-slow")) {
                        cancelSlow();
                        return;
                    }
                    if (target.path() == QStringLiteral("/speed-peer-error") && bytes == 2 * 1024 * 1024) {
                        failingRound.append(socket);
                        if (failingRound.size() == 2) {
                            failingRound.front()->write("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n");
                            failingRound.front()->disconnectFromHost();
                        }
                        return;
                    }
                    if (target.path() == QStringLiteral("/speed-error")) {
                        socket->write("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }
                    if (target.path() == QStringLiteral("/speed-redirect")) {
                        socket->write("HTTP/1.1 302 Found\r\nLocation: /private\r\nContent-Length: 0\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }
                    if (target.path() == QStringLiteral("/speed-truncated")) {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(bytes)
                            + "\r\nConnection: close\r\n\r\nshort");
                        socket->disconnectFromHost();
                        return;
                    }
                    if (target.path() == QStringLiteral("/speed-oversized")) {
                        socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
                        socket->write(QByteArray(bytes + 1, 'x'));
                        socket->disconnectFromHost();
                        return;
                    }
                    // First-byte warmup exceeds 20 ms; larger samples take longer,
                    // so simultaneous lanes have a real wall-clock benefit.
                    QTimer::singleShot(40 + bytes / (32 * 1024), socket, [socket, bytes] {
                        socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(bytes)
                            + "\r\nSet-Cookie: speed=private\r\nConnection: close\r\n\r\n");
                        socket->write(QByteArray(bytes, 'x'));
                        socket->disconnectFromHost();
                    });
                    return;
                }
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

    const auto speedCode = [&](QVariantMap arguments) {
        try {
            QCoro::waitFor(runtime->call("a", "speedTest", std::move(arguments)));
        } catch (const std::exception& error) {
            return QByteArray(error.what());
        }
        return QByteArray();
    };
    const auto waitForSpeedAbort = [&] {
        QElapsedTimer elapsed;
        elapsed.start();
        while (speedActive != 0 && elapsed.elapsed() < 2000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        require(speedActive == 0, "settled benchmark leaves no live peer requests");
    };
    require(!QCoro::waitFor(runtime->call("a", "speedHosts")).value("sourceCanMeasure").toBool(),
        "persistent source host cannot start operation-scoped benchmarks");
    int speedGuiTurns = 0;
    QTimer speedResponsiveness;
    QObject::connect(&speedResponsiveness, &QTimer::timeout, &app, [&] { ++speedGuiTurns; });
    speedResponsiveness.start(10);
    const auto measurement = QCoro::waitFor(runtime->call("a", "speedTest"));
    speedResponsiveness.stop();
    waitForSpeedAbort();
    require(
        measurement.value("bitrate").toLongLong() >= 1000000 && measurement.value("bitrate").toLongLong() <= 1000000000,
        "successful benchmark returns a bounded usable streaming bitrate");
    const int lanes = measurement.value("parallelRequests").toInt();
    require(lanes == 1 || lanes == 2 || lanes == 4, "benchmark selects a supported lane count");
    require(speedSizes == QList<int> { 524288, 4194304, 2097152, 2097152, 1048576, 1048576, 1048576, 1048576 },
        "warmup and completed 1/2/4 lane rounds use the intended byte budgets");
    require(speedMaximum == 4 && speedGuiTurns >= 5, "lane requests overlap without blocking GUI events");
    require(received.back().contains("Authorization: a-token"), "benchmark passes source authentication headers");
    const auto overlapping = QCoro::waitFor(runtime->call("a", "overlappingSpeedTests"));
    require(overlapping.value("error") == "request_denied" && overlapping.value("bitrate").toLongLong() >= 1000000,
        "overlapping benchmark cannot multiply the operation's request budget");
    waitForSpeedAbort();
    const auto ranged
        = QCoro::waitFor(runtime->call("a", "speedTest", { { "url", origin + "/range" }, { "range", true } }));
    require(ranged.value("bitrate").toLongLong() >= 1000000,
        "an authenticated media resource yields a native throughput ceiling without URL placeholders");
    require(speedCode({ { "url", origin + "/range-ignored" }, { "range", true } }) == "http_200",
        "a server ignoring Range cannot benchmark a full media download");
    require(speedCode({ { "url", origin + "/range-wrong" }, { "range", true } }) == "invalid_sample",
        "a same-size response from the wrong offset is not a valid sample");
    require(speedCode({ { "url", origin + "/range-short-total" }, { "range", true } }) == "invalid_sample",
        "Content-Range total must include every requested byte");
    const qsizetype beforeDenied = received.size();
    require(speedCode({ { "url", "http://127.0.0.1:1/private?bytes={bytes}&nonce={nonce}" } }) == "request_denied",
        "benchmark denies ungranted origins");
    require(speedCode({ { "url", "file:///tmp/sample?bytes={bytes}&nonce={nonce}" } }) == "request_denied",
        "benchmark permits only HTTP(S)");
    require(speedCode({ { "url", origin + "/speed?nonce={nonce}" } }) == "request_denied",
        "benchmark requires a requested-byte placeholder");
    require(speedCode({ { "url", origin + "/speed?bytes={bytes}" } }) == "request_denied",
        "benchmark requires cache-busting nonces");
    require(speedCode({ { "headers", QVariantMap { { "Host", "other.invalid" } } } }) == "header_denied",
        "benchmark cannot override the validated authority");
    require(speedCode({ { "headers", QVariantMap { { "X-Test", "bad\r\nInjected: yes" } } } }) == "header_denied",
        "benchmark rejects header injection");
    require(speedCode({ { "headers", QVariantMap { { "Range", "bytes=0-" } } } }) == "header_denied",
        "providers cannot replace bounded probe ranges with an unbounded download");
    require(received.size() == beforeDenied, "invalid benchmarks send no network requests");
    require(speedCode({ { "path", "speed-error" } }) == "http_401", "benchmark preserves authentication failures");
    require(speedCode({ { "path", "speed-redirect" } }) == "http_302", "benchmark never follows a redirect");
    require(!speedCode({ { "path", "speed-truncated" } }).isEmpty(), "incomplete samples cannot yield a bitrate");
    require(speedCode({ { "path", "speed-oversized" } }) == "response_limit", "benchmark bounds streamed bytes");
    require(speedCode({ { "path", "speed-peer-error" } }) == "http_401", "one failed lane rejects its whole round");
    waitForSpeedAbort();
    cancelSlow = [&] { runtime->cancelScope("a", "speed-cancel"); };
    rejects(runtime->call("a", "speedTest", { { "path", "speed-slow" } }, "speed-cancel"),
        "scope cancellation settles an active benchmark");
    waitForSpeedAbort();
    require(QCoro::waitFor(runtime->call("a", "state")).value("calls").toInt() == 1,
        "benchmark cancellation leaves the source usable");
    cancelSlow = [&] { runtime->removeSource("a"); };
    rejects(
        runtime->call("a", "speedTest", { { "path", "speed-slow" } }), "source removal cancels benchmark transport");
    waitForSpeedAbort();
    QCoro::waitFor(add("a"));

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
