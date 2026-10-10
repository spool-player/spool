#include "ProviderRegistry.h"
#include "SourceHub.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Spool {
namespace {
    const QString RemoteTargets = QStringLiteral("remoteTargets");
    [[noreturn]] void invalid()
    {
        throw std::runtime_error("invalid_remote_response");
    }
    bool present(const QVariantMap& map, const QString& key)
    {
        const auto value = map.value(key);
        return value.isValid() && !value.isNull();
    }
    QString text(const QVariant& value, qsizetype maximum = 4096, bool required = false)
    {
        if (value.metaType().id() != QMetaType::QString || value.toString().size() > maximum
            || (required && value.toString().isEmpty()))
            invalid();
        return value.toString();
    }
    QVariantMap object(const QVariant& value)
    {
        if (value.metaType().id() != QMetaType::QVariantMap)
            invalid();
        return value.toMap();
    }
    QVariantList array(const QVariant& value, int maximum)
    {
        if (value.metaType().id() != QMetaType::QVariantList)
            invalid();
        auto result = value.toList();
        if (result.size() > maximum)
            invalid();
        return result;
    }
    bool boolean(const QVariant& value)
    {
        if (value.metaType().id() != QMetaType::Bool)
            invalid();
        return value.toBool();
    }
    double number(const QVariant& value, double minimum, double maximum, bool integral = false)
    {
        const auto type = value.metaType().id();
        if (type != QMetaType::Double && type != QMetaType::Int && type != QMetaType::LongLong
            && type != QMetaType::UInt && type != QMetaType::ULongLong)
            invalid();
        const double result = value.toDouble();
        if (!std::isfinite(result) || result < minimum || result > maximum
            || (integral && std::trunc(result) != result))
            invalid();
        return result;
    }
    QString ticks(const QVariant& value)
    {
        if (value.metaType().id() != QMetaType::QString)
            throw std::runtime_error("invalid_position");
        const auto result = value.toString();
        bool ok = !result.isEmpty() && result.size() <= 19 && std::all_of(result.begin(), result.end(), [](QChar c) {
            return c >= QLatin1Char('0') && c <= QLatin1Char('9');
        });
        if (ok)
            result.toLongLong(&ok);
        if (!ok)
            throw std::runtime_error("invalid_position");
        return result;
    }
    const QSet<QString>& actions()
    {
        static const QSet<QString> values { "play", "pause", "unpause", "stop", "next", "previous", "seek", "volume",
            "mute", "shuffle", "audioTrack", "subtitleTrack", "repeat", "queuePlay", "queueRemove", "queueMove" };
        return values;
    }
    QStringList commands(const QVariant& value)
    {
        QVariantList list;
        if (value.metaType().id() == QMetaType::QStringList) {
            for (const auto& item : value.toStringList())
                list.append(item);
        } else {
            list = array(value, 128);
        }
        if (list.size() > 128)
            invalid();
        QStringList result;
        for (const auto& entry : list) {
            const auto name = text(entry, 128, true);
            if (actions().contains(name) && !result.contains(name))
                result.append(name);
        }
        return result;
    }
    QString repeat(const QVariant& value)
    {
        const QString result = text(value, 32, true);
        if (result != "RepeatNone" && result != "RepeatAll" && result != "RepeatOne")
            invalid();
        return result;
    }
    QUrl httpUrl(const QString& value)
    {
        const QUrl url(value, QUrl::StrictMode);
        if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.host().contains(QLatin1Char('*'))
            || (url.scheme() != "https" && url.scheme() != "http"))
            invalid();
        return url;
    }
    QString origin(const QString& value)
    {
        auto url = httpUrl(value);
        if (url.hasQuery() || url.hasFragment() || (!url.path().isEmpty() && url.path() != "/"))
            invalid();
        url.setPath({});
        if (url.port() == (url.scheme() == "https" ? 443 : 80))
            url.setPort(-1);
        return url.toString(QUrl::FullyEncoded);
    }
}

bool SourceHub::remoteAvailable(const QString& accountId) const
{
    return source(accountId) && accountEnabled(accountId) && m_registry->hasCapability(accountId, RemoteTargets);
}

QCoro::Task<QVariantList> SourceHub::remoteTargets(QString accountId, QString scope)
{
    if (!remoteAvailable(accountId))
        throw std::runtime_error("unsupported_capability");
    QPointer<SourceHub> guard(this);
    QPointer<Provider> owner(source(accountId));
    const auto response = co_await m_registry->callSource(accountId, "remoteTargets", {}, scope);
    if (!guard || !owner || owner != source(accountId) || !remoteAvailable(accountId))
        throw std::runtime_error("cancelled");
    QVariantList result;
    QSet<QString> seen;
    for (const auto& value : array(response.value("targets"), 128)) {
        const auto row = object(value);
        const QString id = text(row.value("id"), 1024, true);
        if (seen.contains(id))
            invalid();
        seen.insert(id);
        // Session/device correlation is provider-owned. Providers must exclude
        // their own advertised session; a target explicitly equal to our device
        // identifier is also rejected at this boundary.
        if (!m_registry->deviceId().isEmpty() && id == m_registry->deviceId())
            continue;
        const QString mode = text(row.value("queueEditing"), 32, true);
        if (mode != "none" && mode != "replace" && mode != "in-place")
            invalid();
        QVariantMap target { { "id", scoped(accountId, id) }, { "accountId", accountId },
            { "name", text(row.value("name"), 4096, true) }, { "commands", commands(row.value("commands")) },
            { "queueEditing", mode }, { "isLocal", false } };
        if (present(row, "detail"))
            target.insert("detail", text(row.value("detail")));
        if (present(row, "customControls"))
            target.insert("customControls", boolean(row.value("customControls")));
        if (present(row, "origins")) {
            QStringList origins;
            for (const auto& address : array(row.value("origins"), 16)) {
                const auto normalized = origin(text(address, 8192, true));
                if (!origins.contains(normalized))
                    origins.append(normalized);
            }
            target.insert("origins", origins);
        }
        result.append(target);
    }
    co_return result;
}

QCoro::Task<QVariantMap> SourceHub::remoteState(QString targetId, bool connect, QString scope)
{
    const auto account = accountOf(targetId);
    if (!remoteAvailable(account))
        throw std::runtime_error("unsupported_capability");
    QPointer<SourceHub> guard(this);
    QPointer<Provider> owner(source(account));
    QVariantMap arguments { { "targetId", rawId(targetId) } };
    arguments.insert("videoPreviews", m_videoPreviewsEnabled);
    const auto response = co_await m_registry->callSource(
        account, connect ? "remoteConnect" : "remoteState", std::move(arguments), scope);
    if (!guard || !owner || owner != source(account) || !remoteAvailable(account))
        throw std::runtime_error("cancelled");
    const auto status = text(response.value("state"), 32, true);
    if (status != "stopped" && status != "playing" && status != "paused" && status != "buffering" && status != "error")
        invalid();
    QVariantMap state { { "state", status }, { "commands", commands(response.value("commands")) } };
    for (const auto *field : { "positionTicks", "runtimeTicks" })
        if (present(response, QLatin1String(field)))
            state.insert(QLatin1String(field), ticks(response.value(QLatin1String(field))));
    for (const auto *field : { "queueRevision", "currentEntryId" })
        if (present(response, QLatin1String(field)))
            state.insert(QLatin1String(field), text(response.value(QLatin1String(field)), 1024));
    if (present(response, "volume"))
        state.insert("volume", number(response.value("volume"), 0, 100));
    if (present(response, "rate"))
        state.insert("rate", number(response.value("rate"), 0.01, 100));
    for (const auto *field : { "muted", "shuffled" })
        if (present(response, QLatin1String(field)))
            state.insert(QLatin1String(field), boolean(response.value(QLatin1String(field))));
    if (present(response, "repeatMode"))
        state.insert("repeatMode", repeat(response.value("repeatMode")));
    if (present(response, "commandSequence"))
        state.insert("commandSequence",
            QVariant::fromValue(quint64(number(response.value("commandSequence"), 0, 9007199254740991.0, true))));
    for (const auto *field : { "audioTracks", "subtitleTracks" }) {
        if (!present(response, QLatin1String(field)))
            continue;
        QVariantList tracks;
        QSet<QString> ids;
        for (const auto& entry : array(response.value(QLatin1String(field)), 128)) {
            const auto track = object(entry);
            const auto id = text(track.value("id"), 1024, true);
            if (ids.contains(id))
                invalid();
            ids.insert(id);
            tracks.append(QVariantMap { { "id", id }, { "label", text(track.value("label")) },
                { "selected", boolean(track.value("selected")) } });
        }
        state.insert(QLatin1String(field), tracks);
    }
    if (present(response, "item")) {
        const auto raw = object(response.value("item"));
        QVariantMap item;
        item.insert("id", scoped(account, text(raw.value("id"), 1024, true)));
        for (const auto *field : { "title", "type", "seriesName", "album", "albumArtist", "posterTag", "thumbTag",
                 "backdropTag", "seriesPosterTag", "albumPosterTag" })
            if (present(raw, QLatin1String(field)))
                item.insert(QLatin1String(field), text(raw.value(QLatin1String(field))));
        for (const auto *field : { "seriesId", "seasonId", "albumId", "thumbItemId", "backdropItemId" })
            if (present(raw, QLatin1String(field)))
                item.insert(QLatin1String(field), scoped(account, text(raw.value(QLatin1String(field)), 1024)));
        if (present(raw, "entryId"))
            item.insert("entryId", text(raw.value("entryId"), 1024));
        state.insert("item", item);
        state.insert("itemId", item.value("id"));
        state.insert("title", item.value("title"));
        ImageRequest image;
        image.itemId = item.value("id").toString();
        image.tag = item.value("posterTag").toString();
        image.imageType = "Primary";
        image.maxWidth = 600;
        image.format = "jpeg";
        image.quality = 90;
        if (image.tag.isEmpty() && !item.value("thumbTag").toString().isEmpty()) {
            image.tag = item.value("thumbTag").toString();
            image.imageType = "Thumb";
            if (!item.value("thumbItemId").toString().isEmpty())
                image.itemId = item.value("thumbItemId").toString();
        }
        if (image.tag.isEmpty() && !item.value("seriesId").toString().isEmpty()) {
            image.itemId = item.value("seriesId").toString();
            image.tag = item.value("seriesPosterTag").toString();
        }
        if (image.tag.isEmpty() && !item.value("albumId").toString().isEmpty()) {
            image.itemId = item.value("albumId").toString();
            image.tag = item.value("albumPosterTag").toString();
        }
        state.insert("artwork", imageUrl(image));
    }
    const QString prefix = targetId.section(QLatin1Char(':'), 0, 0);
    auto& previews = m_entries[prefix].remotePreviews;
    previews.remove(rawId(targetId));
    if (m_videoPreviewsEnabled && present(response, "preview") && state.contains("item")) {
        const auto preview = object(response.value("preview"));
        QVariantMap descriptor;
        const bool bif = preview.value("format").toString() == QLatin1String("bif");
        if (bif)
            descriptor.insert("format", QStringLiteral("bif"));
        if (!bif) {
            for (const auto *field : { "width", "height", "columns", "rows", "count", "intervalMs" }) {
                const int maximum = QByteArray(field) == "intervalMs" ? 3600000
                    : QByteArray(field) == "count"                    ? 1000000
                                                                      : 16384;
                descriptor.insert(
                    QLatin1String(field), int(number(preview.value(QLatin1String(field)), 1, maximum, true)));
            }
        }
        const qint64 sheetWidth = descriptor.value("width").toLongLong() * descriptor.value("columns").toLongLong();
        const qint64 sheetHeight = descriptor.value("height").toLongLong() * descriptor.value("rows").toLongLong();
        if (sheetWidth > 16384 || sheetHeight > 16384 || sheetWidth * sheetHeight > 64 * 1024 * 1024)
            invalid();
        const QString templateUrl = text(preview.value(bif ? "url" : "urlTemplate"), 16384, true);
        QString resolved = templateUrl;
        if (!bif && !resolved.contains("{index}"))
            invalid();
        resolved.replace("{index}", "0");
        if (resolved.contains(QLatin1Char('{')) || resolved.contains(QLatin1Char('}')))
            invalid();
        const QUrl url = httpUrl(resolved);
        // Substitution may occur in a path/query, never in an authority. This
        // proves every numeric tile index stays on the same approved origin.
        QString other = templateUrl;
        other.replace("{index}", "1");
        const auto otherUrl = httpUrl(other);
        if (url.scheme() != otherUrl.scheme() || url.authority() != otherUrl.authority()
            || !m_registry->accountOriginAllowed(account, url))
            invalid();
        QByteArray headers;
        if (present(preview, "headers")) {
            const auto values = object(preview.value("headers"));
            if (values.size() > 32)
                invalid();
            for (auto it = values.cbegin(); it != values.cend(); ++it) {
                const QString value = text(it.value(), 16384);
                const QByteArray name = it.key().toLatin1();
                if (name.isEmpty() || name.size() > 128
                    || !std::all_of(name.begin(), name.end(),
                        [](unsigned char c) {
                            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                                || QByteArrayView("!#$%&'*+-.^_`|~").contains(c);
                        })
                    || value.contains('\r') || value.contains('\n'))
                    invalid();
                headers += name + ": " + value.toUtf8() + '\n';
            }
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(templateUrl.toUtf8());
        hash.addData(QByteArrayView("\0", 1));
        hash.addData(headers);
        const QString revision = QString::fromLatin1(hash.result().toHex());
        previews.insert(rawId(targetId), { templateUrl, std::move(headers), revision });
        descriptor.insert(bif ? "url" : "urlTemplate",
            QStringLiteral("spool-artwork://account-") + prefix + QStringLiteral("/remote/")
                + QString::fromLatin1(
                    rawId(targetId).toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))
                + (bif ? QStringLiteral("?index=0") : QStringLiteral("?index={index}")) + QStringLiteral("&revision=")
                + revision);
        state.insert("preview", descriptor);
    }
    co_return state;
}

QCoro::Task<QVariantMap> SourceHub::remoteCommand(QString targetId, QVariantMap input, QVariantMap state, QString scope)
{
    const QString account = accountOf(targetId);
    if (!remoteAvailable(account))
        throw std::runtime_error("unsupported_capability");
    const QString action = text(input.value("action"), 128, true);
    if (!actions().contains(action) || !state.value("commands").toStringList().contains(action))
        throw std::runtime_error("remote_command_unavailable");
    QVariantMap command { { "action", action } };
    if (action == "play") {
        const auto ids = input.value("itemIds").toStringList();
        if (ids.isEmpty() || ids.size() > 10000)
            throw std::runtime_error("invalid_remote_command");
        QStringList raw;
        raw.reserve(ids.size());
        for (const auto& id : ids) {
            if (accountOf(id) != account)
                throw std::runtime_error("mixed_source_queue");
            raw.append(rawId(id));
        }
        command.insert("itemIds", raw);
        command.insert("index", int(number(input.value("index"), 0, ids.size() - 1, true)));
        command.insert("positionTicks", ticks(input.value("positionTicks")));
        const QString mode = text(input.value("mode"), 32, true);
        if (mode != "now" && mode != "next" && mode != "last" && mode != "shuffle")
            throw std::runtime_error("invalid_remote_command");
        command.insert("mode", mode);
        if (present(input, "variantId"))
            command.insert("variantId", text(input.value("variantId"), 1024, true));
    } else if (action == "seek") {
        const auto position = ticks(input.value("positionTicks"));
        if (state.contains("runtimeTicks")
            && position.toLongLong() > state.value("runtimeTicks").toString().toLongLong())
            throw std::runtime_error("invalid_position");
        command.insert("positionTicks", position);
    } else if (action == "volume") {
        command.insert("value", number(input.value("value"), 0, 100));
    } else if (action == "mute" || action == "shuffle") {
        command.insert("value", boolean(input.value("value")));
    } else if (action == "repeat") {
        command.insert("mode", repeat(input.value("mode")));
    } else if (action == "audioTrack" || action == "subtitleTrack") {
        const auto id = input.value("trackId");
        if (!id.isValid() || (id.isNull() && action == "audioTrack"))
            throw std::runtime_error("invalid_remote_command");
        if (!id.isNull()) {
            const auto trackId = text(id, 1024, true);
            const auto tracks = state.value(action == "audioTrack" ? "audioTracks" : "subtitleTracks").toList();
            if (std::none_of(tracks.begin(), tracks.end(),
                    [&](const QVariant& row) { return row.toMap().value("id") == trackId; }))
                throw std::runtime_error("invalid_remote_command");
        }
        command.insert("trackId", id.isNull() ? QVariant::fromValue(nullptr) : id);
    } else if (action == "queuePlay" || action == "queueRemove" || action == "queueMove") {
        command.insert("entryId", text(input.value("entryId"), 1024, true));
        if (action == "queueMove") {
            command.insert("index", int(number(input.value("index"), 0, 9999, true)));
            if (!input.value("afterEntryId").isValid())
                throw std::runtime_error("invalid_remote_command");
            const auto after = input.value("afterEntryId");
            command.insert(
                "afterEntryId", after.isNull() ? QVariant::fromValue(nullptr) : QVariant(text(after, 1024, true)));
        }
    }
    auto result = co_await m_registry->callSource(
        account, "remoteCommand", { { "targetId", rawId(targetId) }, { "command", command } }, scope);
    QVariantMap normalized;
    if (present(result, "commandSequence"))
        normalized.insert("commandSequence",
            QVariant::fromValue(quint64(number(result.value("commandSequence"), 0, 9007199254740991.0, true))));
    co_return normalized;
}

QCoro::Task<PagedMovieItems> SourceHub::remoteQueue(QString targetId, std::optional<QString> cursor, QString scope)
{
    const auto account = accountOf(targetId);
    if (!remoteAvailable(account))
        throw std::runtime_error("unsupported_capability");
    QVariantMap arguments { { "targetId", rawId(targetId) }, { "limit", 50 } };
    if (cursor)
        arguments.insert("cursor", *cursor);
    QPointer<SourceHub> guard(this);
    QPointer<Provider> owner(source(account));
    auto page = co_await m_registry->callSourceMediaPage(account, "remoteQueue", arguments, 50, scope);
    if (!guard || !owner || owner != source(account) || !remoteAvailable(account))
        throw std::runtime_error("cancelled");
    PagedMovieItems result;
    result.items = scopedItems(std::move(page.items), account);
    result.nextCursor = std::move(page.cursor);
    result.exhausted = page.exhausted;
    result.limit = 50;
    co_return result;
}

QVariantMap SourceHub::remoteQueueRow(const MovieItem& item) const
{
    ImageRequest image;
    image.itemId = item.id;
    image.tag = item.posterTag;
    image.imageType = "Primary";
    image.maxWidth = 300;
    image.format = "jpeg";
    image.quality = 90;
    if (image.tag.isEmpty() && !item.thumbTag.isEmpty()) {
        image.tag = item.thumbTag;
        image.imageType = "Thumb";
        if (!item.thumbItemId.isEmpty())
            image.itemId = item.thumbItemId;
    }
    if (image.tag.isEmpty() && !item.seriesId.isEmpty()) {
        image.itemId = item.seriesId;
        image.tag = item.seriesPrimaryImageTag;
    }
    if (image.tag.isEmpty() && !item.albumId.isEmpty()) {
        image.itemId = item.albumId;
        image.tag = item.albumPrimaryImageTag;
    }
    return { { "itemId", item.id }, { "entryId", item.playlistItemId }, { "title", item.title },
        { "itemType", item.itemType }, { "artwork", imageUrl(image) } };
}
} // namespace Spool
