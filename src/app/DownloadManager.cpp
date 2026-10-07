#include "DownloadManager.h"
#include "../common/TlsTrust.h"
#include "../platform/AndroidDownloadStorage.h"
#include "../provider/SourceHub.h"
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#if defined(Q_OS_ANDROID)
#include <QCoreApplication>
#include <QJniObject>
#include <QtCore/qnativeinterface.h>
#endif

namespace Spool {
struct DownloadManager::Job {
    QString id;
    QString itemId;
    QString title;
    QString destination;
    QVariantMap option;
    QVariantMap metadata;
    QString path;
    QString temporary;
    QString state = QStringLiteral("preparing");
    QString error;
    qint64 received = 0;
    qint64 total = -1;
    bool stopped = false;
    bool checked = false;
    QByteArray prefix;
    DownloadPlan plan;
    QFile file;
    QPointer<QNetworkReply> reply;
};

bool DownloadManager::supported()
{
    return true;
}
bool DownloadManager::mobile()
{
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS) || defined(Q_OS_TVOS)
    return true;
#else
    return false;
#endif
}
bool DownloadManager::canChooseFolder()
{
#if defined(Q_OS_IOS) || defined(Q_OS_TVOS)
    return false;
#else
    return true;
#endif
}
QString DownloadManager::defaultDestination()
{
    const QString configured = qEnvironmentVariable("SPOOL_DATA_HOME");
    if (!configured.isEmpty())
        return QDir(configured).filePath(QStringLiteral("downloads"));
    // Mobile application storage requires neither a shared-storage permission
    // nor an expiring document-provider grant. iOS AppData is backup-excluded.
#if defined(SPOOL_WEBOS) || defined(Q_OS_TVOS)
    const auto location = QStandardPaths::AppDataLocation;
#else
    const auto location = mobile() ? QStandardPaths::AppDataLocation : QStandardPaths::MoviesLocation;
#endif
    QString root = QStandardPaths::writableLocation(location);
    if (root.isEmpty())
        root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(root).filePath(mobile() ? QStringLiteral("downloads") : QStringLiteral("Spool"));
}
QVariantList DownloadManager::destinationChoices()
{
    QVariantList choices;
#if defined(Q_OS_ANDROID)
    choices.push_back(QVariantMap { { QStringLiteral("label"), QStringLiteral("Internal app storage") },
        { QStringLiteral("url"), QUrl::fromLocalFile(defaultDestination()) } });
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const auto external
        = context.callObjectMethod("getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;", nullptr);
    if (external.isValid()) {
        const QString path = external.callObjectMethod("getAbsolutePath", "()Ljava/lang/String;").toString();
        if (!path.isEmpty())
            choices.push_back(QVariantMap { { QStringLiteral("label"), QStringLiteral("External app storage") },
                { QStringLiteral("url"), QUrl::fromLocalFile(QDir(path).filePath(QStringLiteral("downloads"))) } });
    }
#endif
    return choices;
}
DownloadManager::DownloadManager(SourceHub *sources, QString dataRoot, TlsTrustController *trust, QObject *parent)
    : QObject(parent)
    , m_sources(sources)
    , m_dataRoot(std::move(dataRoot))
    , m_network(new QNetworkAccessManager(this))
    , m_androidStorage(new AndroidDownloadStorage(this))
{
    if (trust)
        trust->attachNetworkAccessManager(m_network, QStringLiteral("Download"));
    m_destination = defaultDestination();
    restore();
    connect(m_androidStorage, &AndroidDownloadStorage::folderSelected, this, &DownloadManager::setDestination);
    connect(m_androidStorage, &AndroidDownloadStorage::problem, this, [this](const QString& problem) {
        m_problem = problem;
        emit changed();
    });
    connect(sources, &SourceHub::browseSourcesChanged, this, [this] {
        for (const auto& id : m_order) {
            const auto job = m_jobs.value(id);
            if (!job->stopped
                && (!m_sources->source(m_sources->accountOf(job->itemId))
                    || m_sources->downloadOptions(job->itemId).isEmpty()))
                finish(job, tr("Account unavailable. Sign in and retry."));
        }
    });
}
DownloadManager::~DownloadManager()
{
    disconnect(m_sources, nullptr, this, nullptr);
    for (const auto& job : m_jobs) {
        if (job->stopped)
            continue;
        job->stopped = true;
        m_sources->cancelDownloadNegotiation(job->itemId, job->id);
        if (job->reply) {
            disconnect(job->reply, nullptr, this, nullptr);
            job->reply->abort();
        }
        job->file.close();
        if (!job->temporary.isEmpty()) {
            if (job->temporary.startsWith(QStringLiteral("content://")))
                AndroidDownloadStorage::removeFile(QUrl(job->temporary));
            else
                QFile::remove(job->temporary);
        }
        job->state = QStringLiteral("failed");
        job->error = tr("Interrupted. Retry to download again.");
    }
    persist();
}
QVariantList DownloadManager::jobs() const
{
    QVariantList rows;
    rows.reserve(m_order.size());
    for (const auto& id : m_order) {
        const auto job = m_jobs.value(id);
        rows.push_back(QVariantMap { { QStringLiteral("id"), id }, { QStringLiteral("itemId"), job->itemId },
            { QStringLiteral("title"), job->title }, { QStringLiteral("state"), job->state },
            { QStringLiteral("error"), job->error }, { QStringLiteral("received"), job->received },
            { QStringLiteral("total"), job->total },
            { QStringLiteral("quality"), job->option.value(QStringLiteral("label")) } });
    }
    return rows;
}

QVariantList DownloadManager::libraryFiles() const
{
    QVariantList files;
    for (const auto& id : m_order) {
        const auto job = m_jobs.value(id);
        if (job->state == QStringLiteral("complete"))
            files.push_back(QVariantMap { { QStringLiteral("path"), job->path },
                { QStringLiteral("metadata"), job->metadata }, { QStringLiteral("size"), job->received } });
    }
    return files;
}
void DownloadManager::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    m_enabled = enabled;
    if (!enabled)
        cancelAll();
    else
        m_problem.clear();
    emit enabledChanged();
    emit changed();
}

void DownloadManager::open(const QString& itemId)
{
    if (!m_enabled && !itemId.isEmpty() && statusFor(itemId).isEmpty()) {
        m_problem = tr("Enable Allow downloads in Settings → Downloads first.");
        emit changed();
        emit toastRequested(m_problem);
        return;
    }
    m_selectionItemId = itemId;
    m_opened = true;
    emit openedChanged();
}
void DownloadManager::close()
{
    m_opened = false;
    m_selectionItemId.clear();
    emit openedChanged();
}
QVariantMap DownloadManager::statusFor(const QString& itemId) const
{
    const auto rows = jobs();
    for (auto it = rows.crbegin(); it != rows.crend(); ++it)
        if (it->toMap().value(QStringLiteral("itemId")).toString() == itemId)
            return it->toMap();
    return {};
}
void DownloadManager::start(const QString& itemId, const QVariantMap& option)
{
    if (!supported() || itemId.isEmpty())
        return;
    if (!m_enabled) {
        m_problem = tr("Enable Allow downloads in Settings → Downloads first.");
        emit changed();
        emit toastRequested(m_problem);
        return;
    }
    const auto status = statusFor(itemId);
    const QString state = status.value(QStringLiteral("state")).toString();
    if (state == QStringLiteral("preparing") || state == QStringLiteral("downloading")
        || state == QStringLiteral("complete"))
        return;
    const auto options = m_sources->downloadOptions(itemId);
    const bool valid = std::any_of(
        options.cbegin(), options.cend(), [&option](const QVariant& value) { return value.toMap() == option; });
    if (!valid) {
        m_problem = tr("Downloads are unavailable for this account.");
        emit changed();
        return;
    }
    auto job = std::make_shared<Job>();
    job->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job->itemId = itemId;
    job->title = tr("Preparing download…");
    job->destination = m_destination;
    job->option = option;
    m_jobs.insert(job->id, job);
    m_order.push_back(job->id);
    m_selectionItemId.clear();
    emit openedChanged();
    if (!persist()) {
        finish(job, m_problem);
        return;
    }
    emit changed();
    prepare(job).then([] {},
        [guard = QPointer<DownloadManager>(this), job](const std::exception& error) {
            if (!guard || job->stopped)
                return;
            if (QByteArrayView(error.what()) == "download_cancelled")
                guard->cancel(job->id);
            else
                guard->finish(job, guard->tr("Could not prepare download. Check the account and server, then retry."));
        });
}
QCoro::Task<void> DownloadManager::prepare(std::shared_ptr<Job> job)
{
    QPointer<DownloadManager> guard(this);
    const MovieItem item = co_await m_sources->fetchItemDetails(job->itemId);
    if (!guard || job->stopped)
        co_return;
    job->title = item.title;
    job->metadata = { { QStringLiteral("title"), item.title }, { QStringLiteral("type"), item.itemType },
        { QStringLiteral("overview"), item.overview }, { QStringLiteral("year"), item.year },
        { QStringLiteral("runtimeTicks"), QString::number(item.runtimeTicks) },
        { QStringLiteral("seriesName"), item.seriesName }, { QStringLiteral("seasonNumber"), item.seasonNumber },
        { QStringLiteral("episodeNumber"), item.episodeNumber },
        { QStringLiteral("resumeTicks"), QString::number(item.resumeTicks) } };
    emit changed();
    const QString destination = job->destination;
    DownloadRequest request { job->itemId,
        job->option.value(QStringLiteral("mode")).toString() == QStringLiteral("transcoded"),
        job->option.value(QStringLiteral("maxBitrate")).toLongLong(),
        job->option.value(QStringLiteral("maxHeight")).toInt(), {} };
    if (item.mediaSources.size() == 1)
        request.variantId = item.mediaSources.front().id;
    DownloadPlan plan = co_await m_sources->negotiateDownload(request, job->id);
    if (!guard)
        co_return;
    if (job->stopped) {
        co_await release(std::move(plan));
        co_return;
    }
    // Only known local-playable file containers can be committed to the library.
    static const QSet<QString> containers { QStringLiteral("mkv"), QStringLiteral("mp4"), QStringLiteral("webm"),
        QStringLiteral("mov"), QStringLiteral("avi"), QStringLiteral("m4v"), QStringLiteral("ts"),
        QStringLiteral("mpg"), QStringLiteral("mpeg"), QStringLiteral("wmv"), QStringLiteral("mp3"),
        QStringLiteral("flac"), QStringLiteral("ogg"), QStringLiteral("opus"), QStringLiteral("m4a"),
        QStringLiteral("wav"), QStringLiteral("aac") };
    const QString suffix = QFileInfo(plan.url.path()).suffix().toLower();
    if (!containers.contains(plan.container) || suffix == QStringLiteral("m3u8") || suffix == QStringLiteral("mpd")
        || !m_sources->downloadOriginAllowed(job->itemId, plan.url) || !plan.url.userInfo().isEmpty()
        || !plan.url.fragment().isEmpty()) {
        job->plan = std::move(plan);
        finish(job, tr("The provider did not offer an approved complete media file."));
        co_return;
    }
    QString name = item.title;
    name.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N} ._-]")), QStringLiteral("_"));
    name = name.left(100).trimmed();
    if (name.isEmpty())
        name = QStringLiteral("Media");
    const QString filename = name + QLatin1Char('-') + job->id + QLatin1Char('.') + plan.container;
    job->metadata.insert(QStringLiteral("container"), plan.container);
    if (AndroidDownloadStorage::isTree(QUrl(destination))) {
        const QUrl document = AndroidDownloadStorage::createFile(QUrl(destination), filename + QStringLiteral(".part"));
        if (document.isEmpty()) {
            job->plan = std::move(plan);
            finish(job, tr("Could not create a file in the selected folder. Choose it again to restore access."));
            co_return;
        }
        job->temporary = document.toString(QUrl::FullyEncoded);
        job->path = filename;
    } else {
        job->path = QDir(destination).filePath(filename);
        job->temporary = job->path + QStringLiteral(".part");
    }
    transfer(job, std::move(plan));
}
QCoro::Task<void> DownloadManager::release(DownloadPlan plan)
{
    try {
        if (!plan.cleanup.isEmpty())
            co_await m_sources->releaseDownload(std::move(plan.cleanup));
    } catch (const std::exception&) {
        // Cleanup cannot change an already committed file or expose provider errors.
    }
}
void DownloadManager::transfer(const std::shared_ptr<Job>& job, DownloadPlan plan)
{
    job->plan = std::move(plan);
    job->total = job->plan.size;
    if (!job->temporary.startsWith(QStringLiteral("content://"))
        && !QDir().mkpath(QFileInfo(job->path).absolutePath())) {
        finish(job, tr("Could not create the download folder."));
        return;
    }
    job->file.setFileName(job->temporary);
    if (!job->file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        finish(job, tr("Could not write to the download folder."));
        return;
    }
    QNetworkRequest request(job->plan.url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setTransferTimeout(60000);
    static const QRegularExpression headerName(QStringLiteral("^[!#$%&'*+.^_`|~0-9A-Za-z-]{1,128}$"));
    for (auto it = job->plan.headers.cbegin(); it != job->plan.headers.cend(); ++it) {
        const auto value = it.value().toString().toUtf8();
        if (!headerName.match(it.key()).hasMatch() || value.contains('\r') || value.contains('\n')) {
            finish(job, tr("The provider returned invalid request headers."));
            return;
        }
        request.setRawHeader(it.key().toLatin1(), value);
    }
    if (!persist()) {
        finish(job, m_problem);
        return;
    }
    job->reply = m_network->get(request);
    job->reply->setReadBufferSize(64 * 1024);
    job->state = QStringLiteral("downloading");
    persist();
    emit changed();
    connect(job->reply, &QNetworkReply::readyRead, this, [this, job] { consume(job); });
    connect(job->reply, &QNetworkReply::downloadProgress, this, [this, job](qint64, qint64 total) {
        if (!job->stopped && total >= 0) {
            job->total = total;
            emit changed();
        }
    });
    connect(job->reply, &QNetworkReply::finished, this, [this, job] {
        if (job->stopped)
            return;
        consume(job);
        if (job->stopped)
            return;
        if (job->reply->error() != QNetworkReply::NoError)
            finish(job, tr("Transfer failed. Check the network and server, then retry."));
        else if (job->received <= 0 || (job->total >= 0 && job->received != job->total)
            || (job->plan.size > 0 && job->received != job->plan.size))
            finish(job, tr("The server returned an incomplete file."));
        else
            finish(job);
    });
}
void DownloadManager::consume(const std::shared_ptr<Job>& job)
{
    if (job->stopped || !job->reply)
        return;
    const int status = job->reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const auto mime = job->reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
    if (status != 200 || mime.contains(QStringLiteral("mpegurl")) || mime.contains(QStringLiteral("dash+xml"))
        || mime.startsWith(QStringLiteral("text/")) || mime.contains(QStringLiteral("json"))) {
        finish(job,
            status == 401 || status == 403 ? tr("Sign in again to download this item.")
                                           : tr("The server did not return a complete media file."));
        return;
    }
    char buffer[64 * 1024];
    while (job->reply->bytesAvailable() > 0) {
        const qint64 count = job->reply->read(buffer, sizeof(buffer));
        if (count <= 0)
            break;
        // Detect disguised playlists before publishing any file as offline media.
        if (!job->checked) {
            job->prefix.append(buffer, static_cast<int>(std::min<qint64>(count, 256 - job->prefix.size())));
            const auto prefix = job->prefix.trimmed();
            if (prefix.startsWith("#EXTM3U") || prefix.startsWith("<?xml") || prefix.startsWith("<MPD")
                || prefix.startsWith("<!DOCTYPE") || prefix.startsWith("<html")) {
                finish(job, tr("Playlists cannot be saved as complete media files."));
                return;
            }
            job->checked = job->prefix.size() >= 256;
        }
        if (job->file.write(buffer, count) != count) {
            finish(job, tr("Could not save the file. Check free space and folder access."));
            return;
        }
        job->received += count;
    }
    emit changed();
}
void DownloadManager::finish(const std::shared_ptr<Job>& job, const QString& error)
{
    if (job->stopped)
        return;
    job->stopped = true;
    job->error = error;
    if (job->reply) {
        disconnect(job->reply, nullptr, this, nullptr);
        if (!job->reply->isFinished())
            job->reply->abort();
        job->reply->deleteLater();
        job->reply = nullptr;
    }
    if (error.isEmpty()) {
        if (!job->file.flush())
            job->error = tr("Could not finish writing the media file.");
        job->file.close();
        if (job->file.error() != QFileDevice::NoError && job->error.isEmpty())
            job->error = tr("Could not close the downloaded media file safely.");
        if (job->error.isEmpty()) {
            if (job->temporary.startsWith(QStringLiteral("content://"))) {
                const QUrl renamed = AndroidDownloadStorage::renameFile(QUrl(job->temporary), job->path);
                if (renamed.isEmpty())
                    job->error = tr("Could not publish the downloaded file in the selected folder.");
                else {
                    job->path = renamed.toString(QUrl::FullyEncoded);
                    job->temporary.clear();
                }
            } else if (!QFile::rename(job->temporary, job->path)) {
                job->error = tr("Could not publish the downloaded file.");
            } else {
                job->temporary.clear();
            }
        }
    } else {
        job->file.close();
    }
    if (!job->temporary.isEmpty()) {
        if (job->temporary.startsWith(QStringLiteral("content://")))
            AndroidDownloadStorage::removeFile(QUrl(job->temporary));
        else
            QFile::remove(job->temporary);
        job->temporary.clear();
    }
    job->state = job->error.isEmpty() ? QStringLiteral("complete") : QStringLiteral("failed");
    release(std::move(job->plan)).then([] { });
    if (!persist() && job->state == QStringLiteral("complete")) {
        if (job->path.startsWith(QStringLiteral("content://")))
            AndroidDownloadStorage::removeFile(QUrl(job->path));
        else
            QFile::remove(job->path);
        job->state = QStringLiteral("failed");
        job->error = m_problem;
    }
    emit changed();
    if (job->state == QStringLiteral("complete")) {
        emit libraryChanged();
        emit toastRequested(tr("%1 downloaded. Available in Downloads offline.").arg(job->title));
    } else if (job->error != tr("Cancelled")) {
        emit toastRequested(tr("Download failed: %1").arg(job->title));
    }
}
void DownloadManager::cancel(const QString& id)
{
    const auto job = m_jobs.value(id);
    if (!job || job->stopped)
        return;
    finish(job, tr("Cancelled"));
    job->state = QStringLiteral("cancelled");
    m_sources->cancelDownloadNegotiation(job->itemId, job->id);
    persist();
    emit changed();
}
void DownloadManager::retry(const QString& id)
{
    const auto job = m_jobs.value(id);
    if (!job || !job->stopped || job->state == QStringLiteral("complete"))
        return;
    const QString itemId = job->itemId;
    const QVariantMap option = job->option;
    start(itemId, option);
    if (statusFor(itemId).value(QStringLiteral("id")).toString() != id)
        remove(id);
}
void DownloadManager::remove(const QString& id)
{
    const auto job = m_jobs.value(id);
    if (!job || !job->stopped)
        return;
    const bool content = job->path.startsWith(QStringLiteral("content://"));
    if (job->state == QStringLiteral("complete")
        && !(content ? AndroidDownloadStorage::removeFile(QUrl(job->path)) : QFile::remove(job->path))
        && (content || QFileInfo::exists(job->path))) {
        m_problem = tr("Could not remove the downloaded file.");
        emit changed();
        return;
    }
    if (!job->path.isEmpty() && !content)
        QFile::remove(job->path + QStringLiteral(".spool.json"));
    m_jobs.remove(id);
    m_order.removeAll(id);
    persist();
    emit changed();
    emit libraryChanged();
}
void DownloadManager::cancelAll()
{
    const auto ids = m_order;
    for (const auto& id : ids)
        cancel(id);
}
void DownloadManager::clearFinished()
{
    const auto ids = m_order;
    for (const auto& id : ids)
        if (m_jobs.value(id)->stopped && m_jobs.value(id)->state != QStringLiteral("complete"))
            remove(id);
}
QString DownloadManager::offlineItemId(const QString& id) const
{
    const auto job = m_jobs.value(id);
    if (!job || job->state != QStringLiteral("complete"))
        return {};
    const QString canonical
        = job->path.startsWith(QStringLiteral("content://")) ? job->path : QFileInfo(job->path).canonicalFilePath();
    if (canonical.isEmpty())
        return {};
    return m_sources->scoped(QStringLiteral("spool-downloads"),
        QString::fromLatin1(QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha256).toHex()));
}
void DownloadManager::setDestination(const QUrl& folder)
{
    if (!supported() || (!folder.isLocalFile() && !AndroidDownloadStorage::isTree(folder)))
        return;
    if (AndroidDownloadStorage::isTree(folder)) {
        const auto previous = m_destination;
        m_destination = folder.toString(QUrl::FullyEncoded);
        if (!persist())
            m_destination = previous;
        emit changed();
        return;
    }
    const QString path = QDir::cleanPath(folder.toLocalFile());
    if (mobile()) {
        const auto choices = destinationChoices();
        if (!std::any_of(choices.cbegin(), choices.cend(),
                [&folder](const QVariant& row) { return row.toMap().value(QStringLiteral("url")).toUrl() == folder; }))
            return;
    }
    if (!QFileInfo(path).isAbsolute() || !QDir().mkpath(path)) {
        m_problem = tr("The download folder is unavailable.");
    } else {
        const QString previous = m_destination;
        m_destination = path;
        if (!persist())
            m_destination = previous;
    }
    emit changed();
}
void DownloadManager::resetDestination()
{
    const auto previous = m_destination;
    m_destination = defaultDestination();
    if (!persist())
        m_destination = previous;
    emit changed();
}
void DownloadManager::chooseFolder()
{
    m_androidStorage->chooseFolder();
}
bool DownloadManager::persist()
{
    QDir().mkpath(m_dataRoot);
    QJsonArray rows;
    for (const auto& id : m_order) {
        const auto job = m_jobs.value(id);
        rows.push_back(
            QJsonObject::fromVariantMap({ { QStringLiteral("id"), id }, { QStringLiteral("itemId"), job->itemId },
                { QStringLiteral("title"), job->title }, { QStringLiteral("option"), job->option },
                { QStringLiteral("path"), job->path }, { QStringLiteral("temporary"), job->temporary },
                { QStringLiteral("metadata"), job->metadata }, { QStringLiteral("state"), job->state },
                { QStringLiteral("error"), job->error }, { QStringLiteral("received"), QString::number(job->received) },
                { QStringLiteral("total"), QString::number(job->total) } }));
    }
    const QByteArray data = QJsonDocument(
        QJsonObject { { QStringLiteral("version"), 1 }, { QStringLiteral("destination"), m_destination },
            { QStringLiteral("jobs"),
                rows } }).toJson(QJsonDocument::Compact);
    QSaveFile file(QDir(m_dataRoot).filePath(QStringLiteral("downloads.json")));
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        m_problem = tr("Could not save the download list.");
        return false;
    }
    m_problem.clear();
    return true;
}
void DownloadManager::restore()
{
    QFile file(QDir(m_dataRoot).filePath(QStringLiteral("downloads.json")));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024)
        return;
    const auto root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value(QStringLiteral("version")).toInt() != 1)
        return;
    const QString savedDestination = root.value(QStringLiteral("destination")).toString();
    if (!savedDestination.isEmpty()
        && (QFileInfo(savedDestination).isAbsolute() || AndroidDownloadStorage::isTree(QUrl(savedDestination))))
        m_destination = savedDestination;
    for (const auto& value : root.value(QStringLiteral("jobs")).toArray()) {
        const auto row = value.toObject();
        auto job = std::make_shared<Job>();
        job->id = row.value(QStringLiteral("id")).toString();
        job->itemId = row.value(QStringLiteral("itemId")).toString();
        job->title = row.value(QStringLiteral("title")).toString();
        job->option = row.value(QStringLiteral("option")).toObject().toVariantMap();
        job->path = row.value(QStringLiteral("path")).toString();
        job->temporary = row.value(QStringLiteral("temporary")).toString();
        job->metadata = row.value(QStringLiteral("metadata")).toObject().toVariantMap();
        job->state = row.value(QStringLiteral("state")).toString();
        job->error = row.value(QStringLiteral("error")).toString();
        job->received = row.value(QStringLiteral("received")).toString().toLongLong();
        job->total = row.value(QStringLiteral("total")).toString().toLongLong();
        job->stopped = true;
        if (QUuid(job->id).isNull() || job->itemId.isEmpty() || m_jobs.contains(job->id))
            continue;
        if (!job->temporary.isEmpty()) {
            if (job->temporary.startsWith(QStringLiteral("content://")))
                AndroidDownloadStorage::removeFile(QUrl(job->temporary));
            else
                QFile::remove(job->temporary);
            job->temporary.clear();
        }
        QFile existing(job->path);
        if ((job->state == QStringLiteral("complete") && !existing.open(QIODevice::ReadOnly))
            || job->state == QStringLiteral("preparing") || job->state == QStringLiteral("downloading")) {
            job->state = QStringLiteral("failed");
            job->error = tr("Interrupted or file unavailable. Retry to download again.");
        }
        m_jobs.insert(job->id, job);
        m_order.push_back(job->id);
    }
}
} // namespace Spool

QVariantList Spool::DownloadManager::folderEntries(const QString& path) const
{
    QVariantList rows;
    if (!canChooseFolder() || mobile() || !QFileInfo(path).isAbsolute() || !QFileInfo(path).isDir())
        return rows;
    QDir directory(path);
    if (directory.cdUp())
        rows.push_back(QVariantMap { { QStringLiteral("id"), directory.absolutePath() },
            { QStringLiteral("label"), tr("Up a folder") }, { QStringLiteral("path"), directory.absolutePath() } });
    const auto entries = QDir(path).entryInfoList(QDir::Dirs | QDir::Readable | QDir::NoDotAndDotDot, QDir::Name);
    rows.reserve(rows.size() + entries.size());
    for (const auto& entry : entries)
        rows.push_back(QVariantMap { { QStringLiteral("id"), entry.absoluteFilePath() },
            { QStringLiteral("label"), entry.fileName() }, { QStringLiteral("path"), entry.absoluteFilePath() } });
    return rows;
}
