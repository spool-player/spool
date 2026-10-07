#include "LocalCommandServer.h"
#include "LocalControlProtocol.h"
#include "app/AppController.h"
#include "app/DownloadManager.h"
#include "app/RouterController.h"
#include "app/SettingsSchema.h"
#include "common/AsyncTask.h"
#include "platform/NativeAppWindow.h"
#include "provider/SourceHub.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputMethodEvent>
#include <QInputMethodQueryEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLocalSocket>
#include <QLockFile>
#include <QMouseEvent>
#include <QNetworkProxy>
#include <QPointer>
#include <QSaveFile>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <cmath>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Spool {
namespace {
    const QStringList SettingKeys { QStringLiteral("playback/seekPreviews"),
        QStringLiteral("playback/accurateTrickplay"), QStringLiteral("playback/trickplayPreviewScalePercent"),
        QStringLiteral("appearance/uiScalePercent"), QStringLiteral("playback/renderQuality") };
    QJsonObject itemJson(const MovieItem& item, int index)
    {
        return { { QStringLiteral("index"), index }, { QStringLiteral("id"), item.id },
            { QStringLiteral("title"), item.title }, { QStringLiteral("type"), item.itemType },
            { QStringLiteral("playable"), item.isPlayable() } };
    }
    void closeSocket(QIODevice *socket, bool abort)
    {
        if (auto *local = qobject_cast<QLocalSocket *>(socket)) {
            if (abort)
                local->abort();
            else
                local->disconnectFromServer();
        } else if (auto *tcp = qobject_cast<QTcpSocket *>(socket)) {
            if (abort)
                tcp->abort();
            else
                tcp->disconnectFromHost();
        }
    }
}

LocalCommandServer::LocalCommandServer(AppController *app, RouterController *router, NativeAppWindow *window,
    DownloadManager *downloads, SourceHub *sources)
    : m_app(app)
    , m_router(router)
    , m_window(window)
    , m_downloads(downloads)
    , m_sources(sources)
{
    m_inputClock.start();
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    m_server.setMaxPendingConnections(8);
    connect(&m_server, &QLocalServer::newConnection, this, &LocalCommandServer::acceptConnections);
    m_tcpServer.setProxy(QNetworkProxy::NoProxy);
    m_tcpServer.setMaxPendingConnections(8);
    connect(&m_tcpServer, &QTcpServer::newConnection, this, &LocalCommandServer::acceptConnections);
}
LocalCommandServer::~LocalCommandServer()
{
    stop();
}

bool LocalCommandServer::start(const QString& requestedInstance, QString *error)
{
    return startInternal(requestedInstance, -1, error);
}

bool LocalCommandServer::startTcp(const QString& requestedInstance, quint16 port, QString *error)
{
    return startInternal(requestedInstance, port, error);
}

bool LocalCommandServer::startInternal(const QString& requestedInstance, int tcpPort, QString *error)
{
    const QString directory = LocalControl::directory(error);
    if (directory.isEmpty())
        return false;
    m_instance = requestedInstance.isEmpty() ? QStringLiteral("%1-%2")
                                                   .arg(QCoreApplication::applicationPid())
                                                   .arg(QUuid::createUuid().toString(QUuid::Id128).left(8))
                                             : requestedInstance;
    if (!LocalControl::validInstance(m_instance)) {
        *error = QStringLiteral("Instance must be 1-64 ASCII letters, digits, dots, underscores or hyphens");
        return false;
    }
    m_lock = std::make_unique<QLockFile>(QDir(directory).filePath(m_instance + QStringLiteral(".lock")));
    if (!m_lock->tryLock(0)) {
        *error = QStringLiteral("Instance identifier is already in use");
        return false;
    }
    QString endpoint = tcpPort < 0 ? LocalControl::localEndpoint(directory, m_instance, error)
                                  : QStringLiteral("127.0.0.1");
    if (endpoint.isEmpty()) {
        m_lock.reset();
        return false;
    }
#ifndef Q_OS_UNIX
    if (tcpPort < 0)
        endpoint += QLatin1Char('-') + QUuid::createUuid().toString(QUuid::Id128);
#endif
#ifdef Q_OS_UNIX
    if (tcpPort < 0) {
        struct stat address {};
        if (lstat(QFile::encodeName(endpoint).constData(), &address) == 0) {
            if (!S_ISSOCK(address.st_mode) || address.st_uid != geteuid()) {
                *error = QStringLiteral("The reserved control address is not an owned socket");
                m_lock.reset();
                return false;
            }
            QLocalSocket live;
            live.connectToServer(endpoint);
            if (live.waitForConnected(100)) {
                *error = QStringLiteral("The reserved control address is already listening");
                m_lock.reset();
                return false;
            }
            // The selected registry/instance lock is held. Remove only its
            // unreachable, owned socket left by an interrupted process.
            if (!QLocalServer::removeServer(endpoint)) {
                *error = QStringLiteral("Cannot remove the stale owned control socket");
                m_lock.reset();
                return false;
            }
        } else if (errno != ENOENT) {
            *error = QStringLiteral("Cannot inspect the reserved control address");
            m_lock.reset();
            return false;
        }
    }
#endif
    const bool listening = tcpPort < 0 ? m_server.listen(endpoint)
                                      : m_tcpServer.listen(QHostAddress::LocalHost, quint16(tcpPort));
    if (!listening) {
        *error = QStringLiteral("Cannot listen on the private local socket");
        m_lock.reset();
        return false;
    }
    m_token = QUuid::createUuid().toString(QUuid::Id128) + QUuid::createUuid().toString(QUuid::Id128);
    m_descriptorPath = QDir(directory).filePath(m_instance + QStringLiteral(".json"));
    QSaveFile descriptor(m_descriptorPath);
    if (!descriptor.open(QIODevice::WriteOnly)
        || !descriptor.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        *error = QStringLiteral("Cannot create a private discovery file");
        stop();
        return false;
    }
    QJsonObject data { { QStringLiteral("instance"), m_instance }, { QStringLiteral("endpoint"), endpoint },
        { QStringLiteral("token"), m_token }, { QStringLiteral("pid"), QCoreApplication::applicationPid() } };
    if (tcpPort >= 0) {
        data.insert(QStringLiteral("transport"), QStringLiteral("tcp"));
        data.insert(QStringLiteral("endpoint"), QStringLiteral("127.0.0.1"));
        data.insert(QStringLiteral("port"), m_tcpServer.serverPort());
    }
    const QByteArray bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
    if (descriptor.write(bytes) != bytes.size() || !descriptor.commit()) {
        *error = QStringLiteral("Cannot publish instance discovery");
        stop();
        return false;
    }
    return true;
}

void LocalCommandServer::stop()
{
    m_server.close();
    m_tcpServer.close();
    const auto connections = m_connections;
    for (QIODevice *socket : connections)
        closeSocket(socket, true);
    if (!m_descriptorPath.isEmpty()) {
        QFile::remove(m_descriptorPath);
        m_descriptorPath.clear();
    }
    m_lock.reset();
}

void LocalCommandServer::acceptConnections()
{
    while (m_server.hasPendingConnections())
        acceptSocket(m_server.nextPendingConnection());
    while (m_tcpServer.hasPendingConnections())
        acceptSocket(m_tcpServer.nextPendingConnection());
}

void LocalCommandServer::acceptSocket(QIODevice *socket)
{
    socket->setParent(this);
    if (m_connections.size() >= 8) {
        closeSocket(socket, true);
        socket->deleteLater();
        return;
    }
    m_connections.insert(socket);
    const auto disconnected = [this, socket] {
        m_connections.remove(socket);
        socket->deleteLater();
    };
    if (auto *local = qobject_cast<QLocalSocket *>(socket)) {
        local->setReadBufferSize(LocalControl::MaxRequestBytes + 1);
        connect(local, &QLocalSocket::disconnected, this, disconnected);
    } else if (auto *tcp = qobject_cast<QTcpSocket *>(socket)) {
        tcp->setReadBufferSize(LocalControl::MaxRequestBytes + 1);
        connect(tcp, &QTcpSocket::disconnected, this, disconnected);
    }
    QTimer::singleShot(LocalControl::RequestTimeoutMs, socket, [this, socket] {
        reject(socket, socket->property("requestId").toString(), QStringLiteral("timeout"),
            QStringLiteral("Request did not complete before the server deadline"));
        closeSocket(socket, true);
    });
    auto buffer = std::make_shared<QByteArray>();
    connect(socket, &QIODevice::readyRead, socket, [this, socket, buffer] {
        if (socket->property("dispatched").toBool()) {
            if (socket->bytesAvailable() > 0)
                closeSocket(socket, true);
            return;
        }
        *buffer += socket->readAll();
        if (buffer->size() > LocalControl::MaxRequestBytes) {
            reject(socket, {}, QStringLiteral("request_too_large"), QStringLiteral("Request exceeds 64 KiB"));
            return;
        }
        const qsizetype newline = buffer->indexOf('\n');
        if (newline < 0)
            return;
        socket->setProperty("dispatched", true);
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(buffer->left(newline), &error);
        if (newline != buffer->size() - 1 || error.error != QJsonParseError::NoError || !document.isObject()) {
            reject(socket, {}, QStringLiteral("invalid_request"), QStringLiteral("Expected one newline-framed JSON object"));
            return;
        }
        const QJsonObject request = document.object();
        const QString id = request.value(QStringLiteral("id")).toString();
        socket->setProperty("requestId", id);
        if (id.isEmpty() || id.size() > 128 || !request.value(QStringLiteral("args")).isObject()
            || !request.value(QStringLiteral("command")).isString()) {
            reject(socket, id, QStringLiteral("invalid_request"), QStringLiteral("Missing or invalid id, command or args"));
            return;
        }
        if (request.value(QStringLiteral("token")).toString() != m_token) {
            reject(socket, id, QStringLiteral("unauthorized"), QStringLiteral("Invalid local capability"));
            return;
        }
        dispatch(socket, request);
    });
}

void LocalCommandServer::finish(QIODevice *socket, const QString& id, const QJsonObject& result)
{
    if (socket->property("finished").toBool())
        return;
    socket->setProperty("finished", true);
    QByteArray bytes = QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n';
    if (bytes.size() > LocalControl::MaxResponseBytes)
        bytes = QJsonDocument(LocalControl::failure(id, QStringLiteral("response_too_large"),
                                  QStringLiteral("Result exceeds 1 MiB; request a smaller page")))
                    .toJson(QJsonDocument::Compact)
            + '\n';
    socket->write(bytes);
    closeSocket(socket, false);
}
void LocalCommandServer::reject(QIODevice *socket, const QString& id, const QString& code, const QString& message)
{
    finish(socket, id, LocalControl::failure(id, code, message));
}

QJsonObject LocalCommandServer::state() const
{
    const auto player = m_app->player();
    // Deliberate allowlist: no URLs, provider properties, credentials, logs or arbitrary QML values.
    return { { QStringLiteral("instance"), m_instance }, { QStringLiteral("pid"), QCoreApplication::applicationPid() },
        { QStringLiteral("initialized"), m_app->initialized() }, { QStringLiteral("route"), m_router->route() },
        { QStringLiteral("canBack"), m_router->canPop() }, { QStringLiteral("busy"), m_app->property("busy").toBool() },
        { QStringLiteral("appError"), !m_app->property("errorText").toString().isEmpty() },
        { QStringLiteral("browseLoading"), m_app->browse()->loadingMore() },
        { QStringLiteral("homeLoading"), m_app->home()->loading() },
        { QStringLiteral("visual"), QJsonObject::fromVariantMap(m_window->automationVisualState()) },
        { QStringLiteral("source"), QJsonObject::fromVariantMap(m_app->streamingQualitySource()) },
        { QStringLiteral("playback"),
            QJsonObject { { QStringLiteral("active"), player->sessionActive() },
                { QStringLiteral("loaded"), player->fileLoaded() }, { QStringLiteral("paused"), player->paused() },
                { QStringLiteral("title"), player->title() }, { QStringLiteral("position"), player->positionSeconds() },
                { QStringLiteral("duration"), player->durationSeconds() },
                { QStringLiteral("failed"), !player->errorText().isEmpty() },
                { QStringLiteral("embeddedVideo"), player->embeddedVideoOutput() },
                { QStringLiteral("buffering"), player->buffering() } } },
        { QStringLiteral("window"),
            QJsonObject { { QStringLiteral("width"), m_window->width() },
                { QStringLiteral("height"), m_window->height() }, { QStringLiteral("exposed"), m_window->isExposed() },
                { QStringLiteral("devicePixelRatio"), m_window->devicePixelRatio() } } } };
}

QJsonObject LocalCommandServer::items(const QJsonObject& args) const
{
    const QString kind = args.value(QStringLiteral("kind")).toString(QStringLiteral("browse"));
    const int offset = args.value(QStringLiteral("offset")).toInt(0);
    const int limit = args.value(QStringLiteral("limit")).toInt(100);
    QJsonArray rows;
    int count = 0;
    if (kind == QStringLiteral("libraries")) {
        const auto& libraries = m_app->libraries()->libraries();
        count = int(libraries.size());
        for (int index = offset; index < count && index - offset < limit; ++index) {
            const auto& library = libraries[size_t(index)];
            rows.append(QJsonObject { { QStringLiteral("index"), index }, { QStringLiteral("id"), library.id },
                { QStringLiteral("title"), library.name }, { QStringLiteral("type"), library.collectionType } });
        }
    } else {
        MovieGridModel *model = kind == QStringLiteral("resume") ? m_app->home()->resumeItems()
            : kind == QStringLiteral("next-up")                  ? m_app->home()->nextUpItems()
                                                                 : m_app->browse()->items();
        count = model->count();
        for (int index = offset; index < count && index - offset < limit; ++index)
            rows.append(itemJson(model->movieAt(index), index));
    }
    return { { QStringLiteral("kind"), kind }, { QStringLiteral("offset"), offset }, { QStringLiteral("count"), count },
        { QStringLiteral("items"), rows },
        { QStringLiteral("hasMore"), kind == QStringLiteral("browse") && m_app->browse()->hasMore() } };
}

void LocalCommandServer::screenshot(QIODevice *socket, const QString& id, const QString& path)
{
    if (!m_window->isExposed() || m_window->width() <= 0 || m_window->height() <= 0) {
        reject(socket, id, QStringLiteral("not_exposed"),
            QStringLiteral("Window must be exposed to capture a rendered frame"));
        return;
    }
    // Swap notification originates on the render thread. Readback and saving run on the GUI thread, after that frame.
    connect(
        m_window, &QQuickWindow::frameSwapped, socket,
        [this, socket, id, path] {
            if (socket->property("finished").toBool() || !socket->isOpen())
                return;
            const QImage image = m_window->grabWindow();
            if (image.isNull()) {
                reject(socket, id, QStringLiteral("capture_failed"),
                    QStringLiteral("Qt framebuffer readback returned no image"));
                return;
            }
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly)
                || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) || !image.save(&file, "PNG")
                || !file.commit()) {
                reject(socket, id, QStringLiteral("save_failed"),
                    QStringLiteral("Could not atomically save the PNG file"));
                return;
            }
            const bool videoIncluded = !m_app->player()->sessionActive()
                || m_app->player()->mediaKind() != QStringLiteral("video") || m_app->player()->embeddedVideoOutput();
            const QJsonObject result { { QStringLiteral("path"), path }, { QStringLiteral("width"), image.width() },
                { QStringLiteral("height"), image.height() }, { QStringLiteral("bytes"), QFileInfo(path).size() },
                { QStringLiteral("format"), QStringLiteral("PNG") }, { QStringLiteral("frameAwaited"), true },
                { QStringLiteral("videoIncluded"), videoIncluded },
                { QStringLiteral("capture"), QStringLiteral("QQuickWindow::grabWindow after frameSwapped") },
                { QStringLiteral("limitation"),
                    videoIncluded
                        ? QStringLiteral("Qt window framebuffer, including embedded scene-graph video; PNG is not a "
                                         "calibrated HDR capture")
                        : QStringLiteral("Video is on a separate native plane; this captures the Qt UI only") },
                { QStringLiteral("state"), state() } };
            finish(socket, id,
                { { QStringLiteral("id"), id }, { QStringLiteral("ok"), true }, { QStringLiteral("result"), result } });
        },
        Qt::ConnectionType(Qt::QueuedConnection | Qt::SingleShotConnection));
    m_window->requestUpdate();
}

void LocalCommandServer::dispatch(QIODevice *socket, const QJsonObject& request)
{
    const QString id = request.value(QStringLiteral("id")).toString();
    const QString command = request.value(QStringLiteral("command")).toString();
    const QJsonObject args = request.value(QStringLiteral("args")).toObject();
    const auto done = [this, socket, id](const QJsonObject& result) {
        finish(socket, id,
            { { QStringLiteral("id"), id }, { QStringLiteral("ok"), true }, { QStringLiteral("result"), result } });
    };
    const auto invalid = [this, socket, id](const QString& message) {
        reject(socket, id, QStringLiteral("invalid_argument"), message);
    };
    if (command == QStringLiteral("status") || command == QStringLiteral("state")) {
        done(state());
        return;
    }
    if (!m_app->initialized()) {
        reject(socket, id, QStringLiteral("not_ready"), QStringLiteral("Application initialization has not completed"));
        return;
    }
    if (command == QStringLiteral("items")) {
        const QString kind = args.value(QStringLiteral("kind")).toString(QStringLiteral("browse"));
        if (!QStringList { QStringLiteral("browse"), QStringLiteral("libraries"), QStringLiteral("resume"),
                QStringLiteral("next-up") }
                .contains(kind)
            || args.value(QStringLiteral("offset")).toInt(0) < 0 || args.value(QStringLiteral("limit")).toInt(100) < 1
            || args.value(QStringLiteral("limit")).toInt(100) > 500) {
            invalid(QStringLiteral("Kind must be browse/libraries/resume/next-up; offset >= 0, limit 1..500"));
            return;
        }
        done(items(args));
    } else if (command == QStringLiteral("downloads")) {
        const QString action = args.value(QStringLiteral("action")).toString(QStringLiteral("list"));
        if ((action == QStringLiteral("start") || action == QStringLiteral("retry")) && !m_downloads->enabled()) {
            invalid(QStringLiteral("Offline downloads are disabled in Settings → Downloads"));
            return;
        }
        if (action == QStringLiteral("list")) {
            QJsonArray jobs;
            for (const QVariant& value : m_downloads->jobs()) {
                const QVariantMap row = value.toMap();
                QJsonObject job;
                for (const auto& key : { "id", "itemId", "title", "state", "received", "total", "quality" })
                    job.insert(QLatin1String(key), QJsonValue::fromVariant(row.value(QLatin1String(key))));
                job.insert(QStringLiteral("failed"),
                    row.value(QStringLiteral("state")).toString() == QStringLiteral("failed"));
                jobs.append(job);
            }
            done({ { QStringLiteral("jobs"), jobs } });
        } else if (action == QStringLiteral("options") || action == QStringLiteral("start")) {
            const QString itemId = args.value(QStringLiteral("itemId")).toString();
            const QVariantList options = m_sources->downloadOptions(itemId);
            if (options.isEmpty()) {
                invalid(QStringLiteral("Select an item from a download-capable account"));
                return;
            }
            if (action == QStringLiteral("options")) {
                QJsonArray rows;
                for (qsizetype index = 0; index < options.size(); ++index) {
                    QJsonObject row = QJsonObject::fromVariantMap(options.at(index).toMap());
                    row.insert(QStringLiteral("index"), int(index));
                    rows.append(row);
                }
                done({ { QStringLiteral("options"), rows } });
            } else {
                const int index = args.value(QStringLiteral("index")).toInt(-1);
                if (index < 0 || index >= options.size()) {
                    invalid(QStringLiteral("Select an index from downloads options"));
                    return;
                }
                m_downloads->start(itemId, options.at(index).toMap());
                done({ { QStringLiteral("accepted"), true },
                    { QStringLiteral("completion"),
                        QStringLiteral("Poll downloads list; provider choice may be required") } });
            }
        } else if (action == QStringLiteral("cancel") || action == QStringLiteral("retry")
            || action == QStringLiteral("remove") || action == QStringLiteral("play")) {
            const QString jobId = args.value(QStringLiteral("jobId")).toString();
            bool found = false;
            for (const QVariant& value : m_downloads->jobs()) {
                if (value.toMap().value(QStringLiteral("id")).toString() == jobId) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                invalid(QStringLiteral("Select a job ID from downloads list"));
                return;
            }
            if (action == QStringLiteral("play")) {
                const QString itemId = m_downloads->offlineItemId(jobId);
                if (itemId.isEmpty()) {
                    invalid(QStringLiteral("Download is not available for offline playback"));
                    return;
                }
                m_app->playLocalItemId(itemId);
            } else if (action == QStringLiteral("cancel")) {
                m_downloads->cancel(jobId);
            } else if (action == QStringLiteral("retry")) {
                m_downloads->retry(jobId);
            } else {
                m_downloads->remove(jobId);
            }
            done({ { QStringLiteral("accepted"), true } });
        } else {
            invalid(QStringLiteral("Unknown downloads action"));
        }
    } else if (command == QStringLiteral("qualities")) {
        const QVariantList inventory = m_app->streamingQualityOptions();
        QJsonArray options;
        for (qsizetype index = 0; index < inventory.size(); ++index) {
            QJsonObject option = QJsonObject::fromVariantMap(inventory.at(index).toMap());
            option.insert(QStringLiteral("index"), int(index));
            options.append(option);
        }
        done({ { QStringLiteral("options"), options },
            { QStringLiteral("source"), QJsonObject::fromVariantMap(m_app->streamingQualitySource()) },
            { QStringLiteral("policy"), m_app->connectionSpeedDescription() }, { QStringLiteral("state"), state() } });
    } else if (command == QStringLiteral("quality")) {
        const int index = args.value(QStringLiteral("index")).toInt(-1);
        const QVariantList options = m_app->streamingQualityOptions();
        if (!m_app->player()->sessionActive() || index < 0 || index >= options.size()) {
            invalid(QStringLiteral("Select an index from qualities while playback is active"));
            return;
        }
        const QVariantMap row = options.at(index).toMap();
        m_app->selectStreamingQuality(
            row.value(QStringLiteral("bitrate")).toLongLong(), row.value(QStringLiteral("height")).toInt());
        done({ { QStringLiteral("accepted"), true }, { QStringLiteral("option"), QJsonObject::fromVariantMap(row) },
            { QStringLiteral("completion"),
                QStringLiteral("Stream renegotiation is asynchronous; poll status and qualities") } });
    } else if (command == QStringLiteral("screenshot")) {
        const QString path = args.value(QStringLiteral("path")).toString();
        if (!QDir::isAbsolutePath(path) || path.size() > 4096) {
            invalid(QStringLiteral("Screenshot requires an absolute output path"));
            return;
        }
        screenshot(socket, id, path);
    } else if (command == QStringLiteral("text")) {
        const QJsonValue value = args.value(QStringLiteral("text"));
        if (!value.isString() || value.toString().size() > 4096) {
            invalid(QStringLiteral("Text must be a string of at most 4096 characters"));
            return;
        }
        QObject *focus = m_window->focusObject();
        if (!focus) {
            reject(socket, id, QStringLiteral("no_text_focus"), QStringLiteral("Focus an editable text field first"));
            return;
        }
        QInputMethodQueryEvent query(Qt::ImEnabled | Qt::ImReadOnly);
        QCoreApplication::sendEvent(focus, &query);
        if (!query.value(Qt::ImEnabled).toBool() || query.value(Qt::ImReadOnly).toBool()) {
            reject(socket, id, QStringLiteral("no_text_focus"), QStringLiteral("Focus an editable text field first"));
            return;
        }
        QInputMethodEvent commit;
        commit.setCommitString(value.toString());
        QCoreApplication::sendEvent(focus, &commit);
        done({ { QStringLiteral("accepted"), commit.isAccepted() } });
    } else if (command == QStringLiteral("preview")) {
        const double seconds = args.value(QStringLiteral("seconds")).toDouble(-1);
        const double duration = m_app->player()->durationSeconds();
        if (!m_app->player()->sessionActive() || !m_app->player()->trickplayAvailable() || !std::isfinite(seconds)
            || seconds < 0 || duration <= 0 || seconds > duration) {
            invalid(QStringLiteral("Preview requires available trickplay and seconds within the active timeline"));
            return;
        }
        const QVariantMap bar = m_window->automationVisualState().value(QStringLiteral("seekBar")).toMap();
        if (bar.isEmpty() || bar.value(QStringLiteral("width")).toDouble() <= 0) {
            reject(socket, id, QStringLiteral("controls_hidden"),
                QStringLiteral("Raise playback controls with key up, then retry preview"));
            return;
        }
        const QPointF local(bar.value(QStringLiteral("x")).toDouble()
                + qBound(0.001, seconds / duration, 0.999) * bar.value(QStringLiteral("width")).toDouble(),
            bar.value(QStringLiteral("y")).toDouble() + bar.value(QStringLiteral("height")).toDouble() / 2.0);
        const QPointF global(m_window->mapToGlobal(local.toPoint()));
        QMouseEvent move(QEvent::MouseMove, local, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(m_window, &move);
        done({ { QStringLiteral("accepted"), true }, { QStringLiteral("seconds"), seconds },
            { QStringLiteral("completion"),
                QStringLiteral(
                    "Actual timeline hover dispatched; poll state.visual.previewTexture.ready before screenshot") },
            { QStringLiteral("state"), state() } });
    } else if (command == QStringLiteral("pointer")) {
        const QString action = args.value(QStringLiteral("action")).toString();
        const double x = args.value(QStringLiteral("x")).toDouble(-1);
        const double y = args.value(QStringLiteral("y")).toDouble(-1);
        if (!QStringList { QStringLiteral("move"), QStringLiteral("click"), QStringLiteral("press"),
                QStringLiteral("release"), QStringLiteral("right-click") }
                .contains(action)
            || !std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x >= m_window->width()
            || y >= m_window->height()) {
            invalid(QStringLiteral("Pointer action must be move/click/press/release/right-click within the window"));
            return;
        }
        const QPointF local(x, y);
        const QPointF global(m_window->mapToGlobal(local.toPoint()));
        QMouseEvent move(QEvent::MouseMove, local, global, Qt::NoButton, m_pointerButtons, Qt::NoModifier);
        move.setTimestamp(ulong(m_inputClock.elapsed()));
        QCoreApplication::sendEvent(m_window, &move);
        if (action == QStringLiteral("click") || action == QStringLiteral("right-click")
            || action == QStringLiteral("press")) {
            const auto button = action == QStringLiteral("right-click") ? Qt::RightButton : Qt::LeftButton;
            if (m_pointerButtons != Qt::NoButton) {
                invalid(QStringLiteral("Release the held pointer button before another press"));
                return;
            }
            m_pointerButtons = button;
            QMouseEvent press(QEvent::MouseButtonPress, local, global, button, m_pointerButtons, Qt::NoModifier);
            press.setTimestamp(ulong(m_inputClock.elapsed()));
            QCoreApplication::sendEvent(m_window, &press);
        }
        if (action == QStringLiteral("click") || action == QStringLiteral("right-click")
            || action == QStringLiteral("release")) {
            const auto button = m_pointerButtons.testFlag(Qt::RightButton) ? Qt::RightButton : Qt::LeftButton;
            m_pointerButtons = Qt::NoButton;
            QMouseEvent release(QEvent::MouseButtonRelease, local, global, button, m_pointerButtons, Qt::NoModifier);
            release.setTimestamp(ulong(m_inputClock.elapsed()));
            QCoreApplication::sendEvent(m_window, &release);
        }
        done(state());
    } else if (command == QStringLiteral("settings-get") || command == QStringLiteral("settings-set")) {
        const QString key = args.value(QStringLiteral("key")).toString();
        if (!key.isEmpty() && !SettingKeys.contains(key)) {
            invalid(QStringLiteral(
                "Only seek preview enable/quality/scale, UI scale and render quality settings are exposed"));
            return;
        }
        if (command == QStringLiteral("settings-get")) {
            QJsonObject values;
            for (const QString& allowed : SettingKeys) {
                if (key.isEmpty() || key == allowed)
                    values.insert(allowed, QJsonValue::fromVariant(m_app->settings()->value(allowed)));
            }
            done({ { QStringLiteral("values"), values } });
            return;
        }
        const SettingSpec *spec = findSettingSpec(key);
        const QVariant value = args.value(QStringLiteral("value")).toVariant();
        if (!spec || !spec->persisted || !settingSupportedOnPlatform(*spec) || !settingAcceptsValue(*spec, value)) {
            invalid(QStringLiteral("Value rejected by the application's settings schema"));
            return;
        }
        if (m_settingPending) {
            reject(socket, id, QStringLiteral("busy"), QStringLiteral("A local setting write is still pending"));
            return;
        }
        m_settingPending = true;
        QObject *operation = new QObject(socket);
        connect(operation, &QObject::destroyed, this, [this] { m_settingPending = false; });
        Async::runScoped(
            operation, m_app->settings()->applyValues({ { key, value } }, ChangeOrigin::User),
            [this, done, operation, key] {
                done({ { QStringLiteral("key"), key },
                    { QStringLiteral("value"), QJsonValue::fromVariant(m_app->settings()->value(key)) } });
                operation->deleteLater();
            },
            [this, socket, id, operation](const std::exception_ptr&) {
                reject(socket, id, QStringLiteral("settings_failed"),
                    QStringLiteral("Application could not commit the setting"));
                operation->deleteLater();
            },
            "local control setting");
    } else if (command == QStringLiteral("library")) {
        if (!m_app->openLibraryById(args.value(QStringLiteral("id")).toString())) {
            invalid(QStringLiteral("Library ID is not in the current qualified library inventory"));
            return;
        }
        m_router->push(QStringLiteral("libraryGrid"),
            { { QStringLiteral("libraryId"), args.value(QStringLiteral("id")).toString() } });
        done(state());
    } else if (command == QStringLiteral("load-more")) {
        m_app->browse()->prefetchNextPage();
        done(state());
    } else if (command == QStringLiteral("navigate") || command == QStringLiteral("home")) {
        const QString route = command == QStringLiteral("home") ? QStringLiteral("home")
                                                                : args.value(QStringLiteral("route")).toString();
        if (!QStringList { QStringLiteral("home"), QStringLiteral("search"), QStringLiteral("settings") }.contains(
                route)) {
            invalid(QStringLiteral("Navigation supports home, search or settings"));
            return;
        }
        m_app->handleLocalControl(
            { { QStringLiteral("command"), QStringLiteral("navigate") }, { QStringLiteral("to"), route } });
        done(state());
    } else if (command == QStringLiteral("back") || command == QStringLiteral("key")) {
        QString name = command == QStringLiteral("back") ? QStringLiteral("back")
                                                         : args.value(QStringLiteral("name")).toString().toLower();
        if (name == QStringLiteral("ok"))
            name = QStringLiteral("select");
        if (!QStringList { QStringLiteral("up"), QStringLiteral("down"), QStringLiteral("left"),
                QStringLiteral("right"), QStringLiteral("select"), QStringLiteral("back"), QStringLiteral("space"),
                QStringLiteral("home"), QStringLiteral("end"), QStringLiteral("pageup"), QStringLiteral("pagedown") }
                .contains(name)) {
            invalid(QStringLiteral("Unsupported navigation key"));
            return;
        }
        m_window->bringToFront();
        // The actual app window is the target even if the WM denies activation to a background CLI.
        const QHash<QString, int> keys { { QStringLiteral("up"), Qt::Key_Up }, { QStringLiteral("down"), Qt::Key_Down },
            { QStringLiteral("left"), Qt::Key_Left }, { QStringLiteral("right"), Qt::Key_Right },
            { QStringLiteral("select"), Qt::Key_Return }, { QStringLiteral("back"), Qt::Key_Back },
            { QStringLiteral("space"), Qt::Key_Space }, { QStringLiteral("home"), Qt::Key_Home },
            { QStringLiteral("end"), Qt::Key_End }, { QStringLiteral("pageup"), Qt::Key_PageUp },
            { QStringLiteral("pagedown"), Qt::Key_PageDown } };
        QKeyEvent press(QEvent::KeyPress, keys.value(name), Qt::NoModifier);
        QKeyEvent release(QEvent::KeyRelease, keys.value(name), Qt::NoModifier);
        QCoreApplication::sendEvent(m_window, &press);
        QCoreApplication::sendEvent(m_window, &release);
        done(state());
    } else if (command == QStringLiteral("play")) {
        if (!m_app->handleLocalControl({ { QStringLiteral("command"), command },
                { QStringLiteral("itemId"), args.value(QStringLiteral("id")).toString() },
                { QStringLiteral("fromStart"), args.value(QStringLiteral("fromStart")).toBool() } })) {
            invalid(QStringLiteral("Play requires an explicit account-qualified item ID from items"));
            return;
        }
        done({ { QStringLiteral("accepted"), true },
            { QStringLiteral("completion"), QStringLiteral("Playback starts asynchronously; poll status") } });
    } else if (command == QStringLiteral("pause") || command == QStringLiteral("resume")
        || command == QStringLiteral("stop") || command == QStringLiteral("seek")) {
        if (!m_app->player()->sessionActive()) {
            reject(socket, id, QStringLiteral("no_playback"), QStringLiteral("No active local playback session"));
            return;
        }
        QVariantMap control { { QStringLiteral("command"),
            command == QStringLiteral("resume") ? QStringLiteral("unpause") : command } };
        if (command == QStringLiteral("seek")) {
            const double seconds = args.value(QStringLiteral("seconds")).toDouble(-1);
            if (!std::isfinite(seconds) || seconds < 0 || seconds > m_app->player()->durationSeconds()) {
                invalid(QStringLiteral("Seek seconds must be within the active timeline"));
                return;
            }
            control.insert(QStringLiteral("positionTicks"), qint64(seconds * 10'000'000.0));
        }
        m_app->handleLocalControl(control);
        done(state());
    } else {
        reject(
            socket, id, QStringLiteral("unknown_command"), QStringLiteral("Unknown local command; see spoolet help"));
    }
}
}
