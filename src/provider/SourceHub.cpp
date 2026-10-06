#include "SourceHub.h"

#include "../common/AsyncTask.h"
#include "PortableProvider.h"
#include "ProviderRegistry.h"

#include <QDebug>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QUuid>

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace Spool {

namespace {
    constexpr qsizetype kPrefix = 8;

    QString prefixOf(const QString& accountId)
    {
        return QString(accountId).remove(QLatin1Char('-')).left(kPrefix);
    }

    // Round-robin, so every account is represented near the top of a row.
    std::vector<MovieItem> interleave(std::vector<std::vector<MovieItem>> lists, int limit)
    {
        std::vector<MovieItem> merged;
        for (size_t row = 0; merged.size() < size_t(limit); ++row) {
            bool any = false;
            for (auto& list : lists) {
                if (row < list.size() && merged.size() < size_t(limit)) {
                    merged.push_back(std::move(list[row]));
                    any = true;
                }
            }
            if (!any)
                break;
        }
        return merged;
    }

    QString sourceDisplayName(const QString& name, const QString& address, const QString& provider)
    {
        const QString trimmed = name.trimmed();
        // Default service names and container/host IDs tell people less than
        // the address. Keep descriptive user-chosen names unchanged.
        static const QRegularExpression generic(
            QStringLiteral("^(?:media|server|media[ _-]*server|jellyfin(?:[ _-]*server)?|"
                           "emby(?:[ _-]*server)?|plex(?:[ _-]*media)?(?:[ _-]*server)?|localhost|default)$"),
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression machine(
            QStringLiteral("^(?:[0-9a-f]{10,64}|[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}|[0-9]{6,})$"),
            QRegularExpression::CaseInsensitiveOption);
        if (!address.isEmpty()
            && (trimmed.isEmpty() || generic.match(trimmed).hasMatch() || machine.match(trimmed).hasMatch()))
            return address;
        return trimmed.isEmpty() ? provider : trimmed;
    }

    // Case, accents and punctuation folded away: "Amélie" finds "amelie!".
    QString folded(const QString& text)
    {
        const QString decomposed = text.normalized(QString::NormalizationForm_KD);
        QString out;
        out.reserve(decomposed.size());
        for (const QChar c : decomposed) {
            if (c.isLetterOrNumber())
                out += c.toCaseFolded();
            else if (c.category() != QChar::Mark_NonSpacing && !out.isEmpty() && !out.endsWith(QLatin1Char(' ')))
                out += QLatin1Char(' ');
        }
        return out.trimmed();
    }

    // Lower is closer: the whole title, its start, the start of each word,
    // anywhere in it, or matched by the server on something else.
    int matchRank(const QString& title, const QString& query)
    {
        static const QStringList articles { QStringLiteral("the "), QStringLiteral("a "), QStringLiteral("an ") };
        if (query.isEmpty() || title.isEmpty())
            return 4;
        QString bare = title;
        for (const QString& article : articles) {
            if (bare.startsWith(article) && !query.startsWith(article)) {
                bare = bare.mid(article.size());
                break;
            }
        }
        if (bare == query)
            return 0;
        if (bare.startsWith(query))
            return 1;
        const QStringList words = bare.split(QLatin1Char(' '));
        const QStringList wanted = query.split(QLatin1Char(' '));
        const bool everyWord = std::all_of(wanted.begin(), wanted.end(), [&](const QString& part) {
            return std::any_of(words.begin(), words.end(), [&](const QString& word) { return word.startsWith(part); });
        });
        if (everyWord)
            return 2;
        return bare.contains(query) ? 3 : 4;
    }

    int itemRank(const MovieItem& item, const QString& query)
    {
        int rank = matchRank(folded(item.title), query);
        // An episode of the show asked for is a good answer, just not as
        // good as one whose own title matches.
        if (rank > 0 && !item.seriesName.isEmpty())
            rank = std::min(rank, matchRank(folded(item.seriesName), query) + 2);
        return std::min(rank, 4);
    }
} // namespace

struct SourceHub::SearchRun {
    std::vector<SearchTarget> plan;
    std::vector<std::vector<MovieItem>> found;
    QString query;
    int limit = 0;
    quint64 serial = 0;
    SearchUpdate update;

    // Rank before merging: the best answer wins deterministically and keeps
    // its account-scoped activation and artwork identities intact.
    std::vector<MovieItem> merged() const
    {
        struct Ranked {
            int rank;
            size_t position;
            size_t target;
            const MovieItem *item;
        };
        std::vector<Ranked> ranked;
        QSet<QString> seen;
        for (size_t target = 0; target < found.size(); ++target) {
            for (size_t position = 0; position < found[target].size(); ++position) {
                const MovieItem& item = found[target][position];
                const QString key = plan[target].server + QLatin1Char('\n') + SourceHub::rawId(item.id);
                if (seen.contains(key))
                    continue;
                seen.insert(key);
                ranked.push_back({ itemRank(item, query), position, target, &item });
            }
        }
        std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
            return std::tie(a.rank, a.position, a.target) < std::tie(b.rank, b.position, b.target);
        });
        struct Identity {
            size_t winner;
            QString type;
            QHash<QString, QString> ids;
            QSet<QString> titles;
        };
        const auto normalizeId = [](const QString& name, QString value) {
            value = value.trimmed();
            if (name == QStringLiteral("imdb") || name == QStringLiteral("tmdb") || name == QStringLiteral("tvdb")) {
                value = value.toLower();
                if (value.startsWith(name + QStringLiteral("://")))
                    value.remove(0, name.size() + 3);
                value = value.section(QLatin1Char('?'), 0, 0).section(QLatin1Char('#'), 0, 0);
                if (value.endsWith(QLatin1Char('/')))
                    value.chop(1);
                if (value.startsWith(QStringLiteral("https://")) || value.startsWith(QStringLiteral("http://"))) {
                    const QUrl url(value);
                    const QString host = url.host();
                    const bool recognized
                        = (name == QStringLiteral("imdb")
                              && (host == QStringLiteral("imdb.com") || host == QStringLiteral("www.imdb.com")))
                        || (name == QStringLiteral("tmdb")
                            && (host == QStringLiteral("themoviedb.org")
                                || host == QStringLiteral("www.themoviedb.org")))
                        || (name == QStringLiteral("tvdb")
                            && (host == QStringLiteral("thetvdb.com") || host == QStringLiteral("www.thetvdb.com")));
                    if (recognized)
                        value = url.path().section(QLatin1Char('/'), -1);
                }
                static const QRegularExpression imdb(QStringLiteral("^tt[0-9]+$"));
                static const QRegularExpression numeric(QStringLiteral("^[0-9]+$"));
                if (name == QStringLiteral("imdb"))
                    return imdb.match(value).hasMatch() ? value : QString();
                if (!numeric.match(value).hasMatch())
                    return QString();
                while (value.size() > 1 && value.front() == QLatin1Char('0'))
                    value.remove(0, 1);
                if (value == QStringLiteral("0"))
                    return QString();
            }
            return value;
        };
        std::vector<Identity> identities;
        identities.reserve(ranked.size());
        for (size_t index = 0; index < ranked.size(); ++index) {
            const MovieItem& item = *ranked[index].item;
            Identity candidate { index, item.itemType.toCaseFolded(), {}, {} };
            const auto addId = [&](const QString& name, const QString& value) {
                const QString key = name.trimmed().toCaseFolded();
                const QString id = normalizeId(key, value);
                if (!key.isEmpty() && !id.isEmpty())
                    candidate.ids.insert(key, id);
            };
            for (auto id = item.externalIds.cbegin(); id != item.externalIds.cend(); ++id)
                addId(id.key(), id.value().toString());
            if (!candidate.ids.contains(QStringLiteral("imdb")))
                addId(QStringLiteral("imdb"), item.imdbId);
            if (!candidate.ids.contains(QStringLiteral("tmdb")))
                addId(QStringLiteral("tmdb"), item.tmdbId);
            // Episode/season names are not identities ("Pilot", "Season 1").
            // Unknown years likewise cannot distinguish remakes reliably.
            if ((candidate.type == QStringLiteral("movie") || candidate.type == QStringLiteral("series"))
                && item.year > 0 && !item.title.isEmpty())
                candidate.titles.insert(item.title.toCaseFolded() + QLatin1Char('\n') + QString::number(item.year));
            for (auto previous = identities.begin(); previous != identities.end();) {
                bool sharedId = false;
                bool conflict = candidate.type != previous->type;
                for (auto id = candidate.ids.cbegin(); !conflict && id != candidate.ids.cend(); ++id) {
                    const auto known = previous->ids.constFind(id.key());
                    if (known != previous->ids.cend()) {
                        conflict = known.value() != id.value();
                        sharedId |= !conflict;
                    }
                }
                const bool sameTitle = candidate.titles.intersects(previous->titles);
                if (conflict || (!sharedId && !sameTitle)) {
                    ++previous;
                    continue;
                }
                candidate.winner = std::min(candidate.winner, previous->winner);
                candidate.ids.insert(previous->ids);
                candidate.titles.unite(previous->titles);
                previous = identities.erase(previous);
                // IDs contributed by this group can connect an earlier group.
                previous = identities.begin();
            }
            identities.push_back(std::move(candidate));
        }
        std::sort(identities.begin(), identities.end(),
            [](const Identity& a, const Identity& b) { return a.winner < b.winner; });
        std::vector<MovieItem> items;
        items.reserve(std::min(identities.size(), size_t(limit)));
        for (const Identity& identity : identities) {
            if (items.size() >= size_t(limit))
                break;
            items.push_back(*ranked[identity.winner].item);
        }
        return items;
    }
};

class SourceHub::Playback final : public PlaybackSource {
public:
    explicit Playback(SourceHub *hub)
        : PlaybackSource(hub)
        , m_hub(hub)
    {
    }

    PlaybackSource *active() const
    {
        Provider *provider = m_hub->source(m_account);
        return provider ? provider->playback() : nullptr;
    }
    QByteArray mediaRequestHeaders() const override
    {
        return active() ? active()->mediaRequestHeaders() : QByteArray();
    }
    QUrl mediaOrigin() const override
    {
        return active() ? active()->mediaOrigin() : QUrl();
    }
    int playbackParallelRequests() const override
    {
        return active() ? active()->playbackParallelRequests() : 2;
    }
    bool signedIn() const override
    {
        return m_hub->signedIn();
    }

    QCoro::Task<PlaybackSession> resolvePlayback(MovieItem item, bool forceTranscode) override
    {
        const QString account = m_hub->accountOf(item.id);
        PlaybackSource *playback = sourceFor(item.id);
        if (!playback)
            throw std::runtime_error("source_unavailable");
        if (account != m_account) {
            if (PlaybackSource *previous = active())
                disconnect(previous, nullptr, this, nullptr);
            m_account = account;
            connect(playback, &PlaybackSource::credentialsChanged, this, &PlaybackSource::credentialsChanged);
            connect(playback, &PlaybackSource::playbackNetworkProfileChanged, this,
                &PlaybackSource::playbackNetworkProfileChanged);
            emit playbackNetworkProfileChanged();
        }
        m_hub->m_playbackAccount = account;
        emit m_hub->streamingQualityChanged();
        item.id = rawId(item.id);
        item.seriesId = rawId(item.seriesId);
        item.seasonId = rawId(item.seasonId);
        item.albumId = rawId(item.albumId);
        PlaybackSession session = co_await playback->resolvePlayback(std::move(item), forceTranscode);
        session.itemId = m_hub->scoped(account, session.itemId);
        auto& entry = m_hub->m_entries[prefixOf(account)];
        entry.playbackPreviews.clear();
        auto& preview = session.trickplay;
        QString resource = preview.format == QLatin1String("bif") ? preview.url : preview.urlTemplate;
        QString first = resource;
        first.replace(QLatin1String("{index}"), QStringLiteral("0"));
        QString next = resource;
        next.replace(QLatin1String("{index}"), QStringLiteral("1"));
        const QUrl firstUrl(first, QUrl::StrictMode);
        const QUrl nextUrl(next, QUrl::StrictMode);
        const auto origin
            = [](const QUrl& url) { return url.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment); };
        if (resource.isEmpty() || !firstUrl.isValid() || !nextUrl.isValid()
            || (firstUrl.scheme() != QLatin1String("http") && firstUrl.scheme() != QLatin1String("https"))
            || origin(firstUrl) != origin(nextUrl) || origin(firstUrl) != playback->mediaOrigin()
            || !m_hub->m_registry->accountOriginAllowed(account, firstUrl)
            || (preview.format != QLatin1String("bif") && !resource.contains(QLatin1String("{index}")))) {
            preview = {};
        } else {
            const QString token = QUuid::createUuid().toString(QUuid::Id128);
            entry.playbackPreviews.insert(
                token, { resource, preview.headers.isEmpty() ? playback->mediaRequestHeaders() : preview.headers, {} });
            const QString scoped = QStringLiteral("spool-artwork://account-") + prefixOf(account)
                + QStringLiteral("/preview/") + token + QStringLiteral("?index=");
            if (preview.format == QLatin1String("bif"))
                preview.url = scoped + QLatin1Char('0');
            else
                preview.urlTemplate = scoped + QStringLiteral("{index}");
            preview.headers.clear();
        }
        for (PlaybackQueueItem& entry : session.nowPlayingQueue)
            entry.itemId = m_hub->scoped(account, entry.itemId);
        co_return session;
    }
    QCoro::Task<std::vector<MediaSegment>> fetchMediaSegments(QString itemId) override
    {
        PlaybackSource *playback = sourceFor(itemId);
        if (!playback)
            co_return {};
        co_return co_await playback->fetchMediaSegments(rawId(itemId));
    }
    QCoro::Task<std::vector<MovieItem>> fetchSeriesEpisodes(QString seriesId) override
    {
        return m_hub->fetchEpisodes(std::move(seriesId));
    }
    QCoro::Task<void> reportPlaybackStart(PlaybackSession session, double rate, int volume, bool muted) override
    {
        PlaybackSource *playback = sourceFor(session.itemId);
        if (!playback)
            co_return;
        session.itemId = rawId(session.itemId);
        co_await playback->reportPlaybackStart(std::move(session), rate, volume, muted);
    }
    QCoro::Task<void> reportPlaybackProgress(
        PlaybackSession session, qint64 positionTicks, bool paused, double rate, int volume, bool muted) override
    {
        PlaybackSource *playback = sourceFor(session.itemId);
        if (!playback)
            co_return;
        session.itemId = rawId(session.itemId);
        co_await playback->reportPlaybackProgress(std::move(session), positionTicks, paused, rate, volume, muted);
    }
    QCoro::Task<void> reportPlaybackStopped(
        PlaybackSession session, qint64 positionTicks, bool failed, double rate) override
    {
        PlaybackSource *playback = sourceFor(session.itemId);
        if (!playback)
            co_return;
        session.itemId = rawId(session.itemId);
        co_await playback->reportPlaybackStopped(std::move(session), positionTicks, failed, rate);
    }

private:
    PlaybackSource *sourceFor(const QString& scopedId) const
    {
        Provider *provider = m_hub->owner(scopedId);
        return provider ? provider->playback() : nullptr;
    }

    SourceHub *m_hub;
    QString m_account;
};

SourceHub::SourceHub(ProviderRegistry *registry, QObject *parent)
    : Provider(parent)
    , m_registry(registry)
    , m_playback(new Playback(this))
{
    m_speedTestTimer.setSingleShot(true);
    m_speedTestTimer.setInterval(5000);
    connect(&m_speedTestTimer, &QTimer::timeout, this, &SourceHub::startNextSpeedTest);
    // Accounts start in parallel at launch; the first home load waits for
    // the burst to settle rather than rebuilding once per account.
    m_settled.setSingleShot(true);
    m_settled.setInterval(150);
    connect(&m_settled, &QTimer::timeout, this, [this] {
        if (!m_announced) {
            m_announced = true;
            emit sessionStarted();
        } else {
            emit contentChanged({});
        }
    });
    connect(registry, &ProviderRegistry::sourceStarted, this, &SourceHub::addSource);
    connect(registry, &ProviderRegistry::sourceStopped, this, &SourceHub::removeSource);
    connect(registry, &ProviderRegistry::accountsChanged, this, &SourceHub::syncBrowse);
    const auto supportChanged = [this](const QString& account) {
        if (account == m_itemActionsAccount)
            cancelItemActions();
        emit extensionSupportChanged(account);
    };
    connect(registry, &ProviderRegistry::extensionsChanged, this, supportChanged);
    connect(registry, &ProviderRegistry::sourceStopped, this, supportChanged);
    connect(registry, &ProviderRegistry::restoredChanged, this, [this] {
        // Nothing to wait for: announce the empty state so the shell moves on.
        if (m_registry->restored()
            && std::none_of(m_registry->accountList().begin(), m_registry->accountList().end(),
                [](const ProviderAccount& account) { return account.enabled; }))
            m_settled.start(0);
    });
}

SourceHub::~SourceHub()
{
    cancelSpeedTest();
}

QString SourceHub::displayName() const
{
    return QStringLiteral("Spool");
}

PlaybackSource *SourceHub::playback()
{
    return m_playback;
}

QVariantList SourceHub::downloadOptions(const QString& itemId) const
{
    const Provider *provider = owner(itemId);
    if (!provider || !accountEnabled(provider->id()) || !provider->capabilities().testFlag(Downloads))
        return {};
    QVariantList options { QVariantMap { { QStringLiteral("label"), QStringLiteral("Original") },
        { QStringLiteral("mode"), QStringLiteral("original") } } };
    if (provider->capabilities().testFlag(DownloadTranscode)) {
        const auto rungs = StreamQualityControl::defaultLadder(0);
        for (size_t index = 0; index < rungs.size(); ++index) {
            const auto& rung = rungs[index];
            if (rung.height > 1080 || (index + 1 < rungs.size() && rungs[index + 1].height == rung.height))
                continue;
            options.push_back(QVariantMap { { QStringLiteral("label"), QStringLiteral("Converted · ") + rung.label },
                { QStringLiteral("mode"), QStringLiteral("transcoded") },
                { QStringLiteral("maxBitrate"), rung.bitrate }, { QStringLiteral("maxHeight"), rung.height } });
        }
    }
    return options;
}

QCoro::Task<DownloadPlan> SourceHub::negotiateDownload(DownloadRequest request, QString scope)
{
    QPointer<Provider> provider = owner(request.itemId);
    if (!provider || !accountEnabled(provider->id()) || !provider->downloads()
        || (request.transcode && !provider->capabilities().testFlag(DownloadTranscode)))
        throw std::runtime_error("download_unavailable");
    const QString account = provider->id();
    request.itemId = rawId(request.itemId);
    DownloadPlan plan = co_await provider->downloads()->negotiateDownload(request, scope);
    if (!plan.cleanup.isEmpty())
        plan.cleanup = { { QStringLiteral("account"), account }, { QStringLiteral("payload"), plan.cleanup } };
    co_return plan;
}

QCoro::Task<void> SourceHub::releaseDownload(QVariantMap cleanup)
{
    Provider *provider = source(cleanup.value(QStringLiteral("account")).toString());
    if (provider && provider->downloads())
        co_await provider->downloads()->releaseDownload(cleanup.value(QStringLiteral("payload")).toMap());
}

bool SourceHub::downloadOriginAllowed(const QString& itemId, const QUrl& url) const
{
    return source(accountOf(itemId)) && accountEnabled(accountOf(itemId))
        && m_registry->accountOriginAllowed(accountOf(itemId), url);
}

void SourceHub::cancelDownloadNegotiation(const QString& itemId, const QString& scope)
{
    m_registry->cancelSourceScope(accountOf(itemId), scope);
}

bool SourceHub::ready() const
{
    return m_registry->restored();
}

void SourceHub::addSource(Provider *provider)
{
    const QString accountId = provider->id();
    const bool browse = accountEnabled(accountId);
    m_entries.insert(prefixOf(accountId), { accountId, provider, browse });
    connect(provider, &Provider::capabilitiesChanged, this, [this, accountId, provider] {
        if (!provider->capabilities().testFlag(Provider::SpeedTest)) {
            if (m_speedTestAccount == accountId)
                cancelSpeedTest();
            auto entry = m_entries.find(prefixOf(accountId));
            if (entry != m_entries.end()) {
                entry->speedState = Entry::SpeedState::Pending;
                entry->measuredBitrate = 0;
                entry->parallelRequests = 2;
            }
            pushPlaybackContext();
        }
        refresh();
    });
    connect(provider, &Provider::contentChanged, this, [this, accountId](const QString& itemId) {
        emit contentChanged(itemId.isEmpty() ? QString() : scoped(accountId, itemId));
    });
    connect(provider, &Provider::sourceEvent, this, [this, accountId](const QString& type, const QVariantMap& payload) {
        if (type == QStringLiteral("remoteChanged")) {
            const auto target = payload.value(QStringLiteral("targetId"));
            if (m_registry->extensionVersion(accountId, QStringLiteral("spool.remote-targets")) != 1
                || target.metaType().id() != QMetaType::QString || target.toString().isEmpty()
                || target.toString().size() > 1024)
                return;
            emit accountEvent(
                accountId, type, { { QStringLiteral("targetId"), scoped(accountId, target.toString()) } });
            return;
        }
        if (type == QStringLiteral("playbackQueueStatus")) {
            const QString state = payload.value(QStringLiteral("state")).toString();
            const QString revision = payload.value(QStringLiteral("revision")).toString();
            if (m_registry->extensionVersion(accountId, QStringLiteral("spool.playback-queue-reporting")) != 1
                || revision.isEmpty()
                || revision != m_queueSnapshots.value(accountId).value(QStringLiteral("revision")).toString()
                || (state != QStringLiteral("preparing") && state != QStringLiteral("ready")
                    && state != QStringLiteral("unavailable")))
                return;
            emit accountEvent(
                accountId, type, { { QStringLiteral("revision"), revision }, { QStringLiteral("state"), state } });
            return;
        }
        emit accountEvent(accountId, type, payload);
    });
    connect(provider, &Provider::errorOccurred, this, &Provider::errorOccurred);
    connect(provider, &Provider::toastRequested, this, &Provider::toastRequested);
    pushPlaybackContext();
    // A set-aside account joining for search changes nothing on screen.
    if (browse) {
        emit browseSourcesChanged();
        refresh();
    }
}

void SourceHub::removeSource(const QString& accountId)
{
    if (!m_entries.contains(prefixOf(accountId)))
        return;
    const Entry entry = m_entries.take(prefixOf(accountId));
    if (m_speedTestAccount == accountId)
        cancelSpeedTest();
    if (m_playbackAccount == accountId) {
        m_playbackAccount.clear();
        emit m_playback->credentialsChanged();
    }
    if (entry.browse) {
        emit browseSourcesChanged();
        refresh();
    }
}

bool SourceHub::accountEnabled(const QString& accountId) const
{
    const auto& accounts = m_registry->accountList();
    const auto found = std::find_if(accounts.begin(), accounts.end(), [&](const auto& a) { return a.id == accountId; });
    // A native source with no saved account (tests, the local folder) is browsed.
    return found == accounts.end() || found->enabled;
}

// Choosing another user of a server promotes an account that was already
// running for search, and sets the previous one aside.
void SourceHub::syncBrowse()
{
    bool changed = false;
    for (Entry& entry : m_entries) {
        const bool browse = accountEnabled(entry.accountId);
        if (entry.browse != browse) {
            changed = true;
            entry.browse = browse;
            if (entry.accountId == m_itemActionsAccount)
                cancelItemActions();
            emit extensionSupportChanged(entry.accountId);
        }
    }
    if (changed) {
        emit browseSourcesChanged();
        refresh();
    }
}

void SourceHub::refresh()
{
    Capabilities capabilities;
    for (Provider *provider : sources())
        capabilities |= provider->capabilities();
    if (capabilities != m_capabilities) {
        m_capabilities = capabilities;
        emit capabilitiesChanged();
    }
    m_settled.start();
    if (!m_speedTestAccount.isEmpty() && !accountEnabled(m_speedTestAccount))
        cancelSpeedTest();
    if (!m_playbackActive && m_speedTestAccount.isEmpty())
        m_speedTestTimer.start(5000);
    emit streamingQualityChanged();
}

QString SourceHub::scoped(const QString& accountId, const QString& rawId) const
{
    return rawId.isEmpty() ? QString() : prefixOf(accountId) + QLatin1Char(':') + rawId;
}

QString SourceHub::accountOf(const QString& scopedId) const
{
    if (scopedId.size() <= kPrefix || scopedId.at(kPrefix) != QLatin1Char(':'))
        return {};
    return m_entries.value(scopedId.left(kPrefix)).accountId;
}

QVariantMap SourceHub::originOf(const QString& scopedId) const
{
    const QString accountId = accountOf(scopedId);
    if (accountId.isEmpty())
        return {};
    for (const QVariant& value : m_registry->accounts()) {
        const QVariantMap account = value.toMap();
        if (account.value(QStringLiteral("id")).toString() != accountId)
            continue;
        const QString provider = account.value(QStringLiteral("providerName")).toString();
        const QString detail = account.value(QStringLiteral("detail")).toString();
        const QString address = account.value(QStringLiteral("address")).toString();
        return { { QStringLiteral("providerName"), provider },
            { QStringLiteral("iconUrl"), account.value(QStringLiteral("iconUrl")) },
            { QStringLiteral("serverName"), sourceDisplayName(detail, address, provider) },
            { QStringLiteral("address"), address },
            { QStringLiteral("userName"), account.value(QStringLiteral("label")) } };
    }
    return {};
}

bool SourceHub::multipleSources() const
{
    return sources().size() > 1;
}

QString SourceHub::rawId(const QString& scopedId)
{
    return scopedId.size() > kPrefix && scopedId.at(kPrefix) == QLatin1Char(':') ? scopedId.mid(kPrefix + 1) : scopedId;
}

Provider *SourceHub::source(const QString& accountId) const
{
    return m_entries.value(prefixOf(accountId)).provider;
}

Provider *SourceHub::owner(const QString& scopedId) const
{
    if (scopedId.size() <= kPrefix || scopedId.at(kPrefix) != QLatin1Char(':'))
        return nullptr;
    return m_entries.value(scopedId.left(kPrefix)).provider;
}

std::vector<Provider *> SourceHub::sources() const
{
    std::vector<Provider *> list;
    for (const Entry& entry : std::as_const(m_entries)) {
        if (entry.provider && entry.browse)
            list.push_back(entry.provider);
    }
    std::sort(list.begin(), list.end(), [](Provider *a, Provider *b) { return a->id() < b->id(); });
    return list;
}

QCoro::Task<QVariantMap> SourceHub::call(QString accountId, QString operation, QVariantMap arguments)
{
    // Server-bound queues arrive scoped. Never turn another account's media
    // into a valid-looking raw ID on the selected server.
    if (operation == QStringLiteral("groupSend") || operation == QStringLiteral("remoteCommand")) {
        const bool remote = operation == QStringLiteral("remoteCommand");
        QVariantMap command = remote ? arguments.value(QStringLiteral("command")).toMap() : arguments;
        if (command.contains(QStringLiteral("itemIds"))) {
            QStringList ids = command.value(QStringLiteral("itemIds")).toStringList();
            for (QString& id : ids) {
                if (accountOf(id) != accountId)
                    throw std::runtime_error("mixed_source_queue");
                id = rawId(id);
            }
            command.insert(QStringLiteral("itemIds"), ids);
            if (remote)
                arguments.insert(QStringLiteral("command"), command);
            else
                arguments = std::move(command);
        }
    }
    auto *portable = qobject_cast<PortableProvider *>(source(accountId));
    if (!portable)
        throw std::runtime_error("source_unavailable");
    co_return co_await portable->call(std::move(operation), std::move(arguments));
}

void SourceHub::setVideoCodecs(QStringList codecs, bool restrict)
{
    m_videoCodecs = std::move(codecs);
    m_restrictVideoCodecs = restrict;
    pushPlaybackContext();
}

QVariantList SourceHub::baselineItemActions(const QString& itemId, const QString& itemType) const
{
    const QString account = accountOf(itemId);
    const auto& accounts = m_registry->accountList();
    const auto owner = std::find_if(accounts.begin(), accounts.end(), [&](const auto& a) { return a.id == account; });
    const ProviderModule *module = owner == accounts.end() ? nullptr : m_registry->module(owner->module);
    QVariantList actions;
    if (!module || module->manifest.extensions.contains(QStringLiteral("spool.item-actions")))
        return actions;
    for (const QVariant& value : module->manifest.actions) {
        const QVariantMap action = value.toMap();
        const QStringList types = action.value(QStringLiteral("types")).toStringList();
        if (types.isEmpty() || types.contains(itemType))
            actions.append(action);
    }
    return actions;
}

QCoro::Task<QVariantList> SourceHub::fetchItemActions(
    QString itemId, QString itemType, QString containerId, QString entryId, QString scope)
{
    const QString account = accountOf(itemId);
    if (!source(account) || !accountEnabled(account))
        throw std::runtime_error("source_unavailable");
    if (!containerId.isEmpty() && accountOf(containerId) != account)
        throw std::runtime_error("mixed_source_collection");
    if (m_registry->extensionVersion(account, QStringLiteral("spool.item-actions")) != 1)
        co_return baselineItemActions(itemId, itemType);
    QVariantMap args { { QStringLiteral("itemId"), rawId(itemId) }, { QStringLiteral("itemType"), itemType } };
    if (!containerId.isEmpty())
        args.insert(QStringLiteral("containerId"), rawId(containerId));
    if (!entryId.isEmpty())
        args.insert(QStringLiteral("entryId"), entryId);
    const auto result = co_await m_registry->callExtension(account, QStringLiteral("spool.item-actions"),
        QStringLiteral("itemActions"), std::move(args), std::move(scope));
    const auto actions = result.value(QStringLiteral("actions")).toList();
    if (result.value(QStringLiteral("actions")).metaType().id() != QMetaType::QVariantList)
        throw std::runtime_error("invalid_item_actions");
    if (actions.size() > 128)
        throw std::runtime_error("response_limit");
    QSet<QString> ids;
    for (const auto& value : actions) {
        const auto action = value.toMap();
        const auto id = action.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || ids.contains(id) || action.value(QStringLiteral("label")).toString().isEmpty())
            throw std::runtime_error("invalid_item_actions");
        if (action.contains(QStringLiteral("enabled"))
            && action.value(QStringLiteral("enabled")).metaType().id() != QMetaType::Bool)
            throw std::runtime_error("invalid_item_actions");
        ids.insert(id);
    }
    co_return actions;
}

int SourceHub::requestItemActions(
    const QString& itemId, const QString& itemType, const QString& containerId, const QString& entryId)
{
    cancelItemActions();
    const int request = m_itemActionsRequest;
    m_itemActionsAccount = accountOf(itemId);
    // Even baseline answers arrive after QML has stored the request identity.
    QTimer::singleShot(0, this, [this, request, itemId, itemType, containerId, entryId] {
        if (request != m_itemActionsRequest)
            return;
        Async::runScoped(
            this, fetchItemActions(itemId, itemType, containerId, entryId, QStringLiteral("item-menu")),
            [this, request](QVariantList actions) {
                if (request == m_itemActionsRequest)
                    emit itemActionsReady(request, actions, {});
            },
            [this, request](const std::exception_ptr&) {
                if (request == m_itemActionsRequest)
                    emit itemActionsReady(
                        request, {}, QStringLiteral("Provider actions are unavailable. Try reopening the menu."));
            },
            "item menu");
    });
    return request;
}

void SourceHub::cancelItemActions()
{
    m_itemActionsRequest = m_itemActionsRequest == std::numeric_limits<int>::max() ? 1 : m_itemActionsRequest + 1;
    if (!m_itemActionsAccount.isEmpty())
        m_registry->cancelSourceScope(m_itemActionsAccount, QStringLiteral("item-menu"));
    m_itemActionsAccount.clear();
}

void SourceHub::runItemAction(const QString& actionId, const QString& itemId, const QString& itemType,
    const QString& containerId, const QString& entryId)
{
    const QString account = accountOf(itemId);
    const auto run = [](SourceHub *self, QString account, QString scopedItemId, QString scopedContainerId,
                         QVariantMap args) -> QCoro::Task<void> {
        QPointer<SourceHub> guard(self);
        QPointer<Provider> sourceGuard(self->source(account));
        const auto actions
            = co_await self->fetchItemActions(scopedItemId, args.value(QStringLiteral("itemType")).toString(),
                scopedContainerId, args.value(QStringLiteral("entryId")).toString(), QStringLiteral("item-action"));
        if (!guard)
            co_return;
        if (!sourceGuard || sourceGuard != self->source(account) || !self->accountEnabled(account))
            throw std::runtime_error("source_unavailable");
        const auto requested = args.value(QStringLiteral("action")).toString();
        const bool permitted = std::any_of(actions.begin(), actions.end(), [&](const QVariant& value) {
            const auto action = value.toMap();
            return action.value(QStringLiteral("id")).toString() == requested
                && action.value(QStringLiteral("enabled"), true).toBool();
        });
        if (!permitted)
            throw std::runtime_error("action_unavailable");
        QVariantMap result = co_await self->call(account, QStringLiteral("runItemAction"), args);
        if (guard && result.contains(QStringLiteral("pick"))) {
            const QVariantMap choice
                = co_await self->m_registry->pick(account, result.value(QStringLiteral("pick")).toMap());
            if (!guard || choice.isEmpty())
                co_return;
            if (!sourceGuard || sourceGuard != self->source(account) || !self->accountEnabled(account))
                throw std::runtime_error("source_unavailable");
            args.insert(choice);
            result = co_await self->call(account, QStringLiteral("runItemAction"), args);
        }
        if (!guard)
            co_return;
        if (result.value(QStringLiteral("changed")).toBool())
            emit self->contentChanged(self->scoped(account, result.value(QStringLiteral("itemId")).toString()));
        if (const QString message = result.value(QStringLiteral("message")).toString(); !message.isEmpty())
            emit self->toastRequested(message);
    };
    Async::runScoped(
        this,
        run(this, account, itemId, containerId,
            { { QStringLiteral("action"), actionId }, { QStringLiteral("itemId"), rawId(itemId) },
                { QStringLiteral("itemType"), itemType }, { QStringLiteral("containerId"), rawId(containerId) },
                { QStringLiteral("entryId"), entryId } }),
        [] {},
        [this](const std::exception_ptr& error) {
            const auto code = exceptionMessage(error);
            emit toastRequested(code.contains(QStringLiteral("http_403"))
                        || code.contains(QStringLiteral("permission_denied"))
                        || code.contains(QStringLiteral("action_unavailable"))
                    ? tr("This action is not permitted for this account.")
                    : tr("The action could not be completed. Reopen the menu to refresh available actions."));
        },
        "item action");
}

bool SourceHub::collectionEditingAvailable(const QString& containerId) const
{
    const auto account = accountOf(containerId);
    return source(account) && accountEnabled(account)
        && m_registry->extensionVersion(account, QStringLiteral("spool.collection-editing")) == 1;
}

QCoro::Task<QVariantMap> SourceHub::collectionCall(QString containerId, QString operation, QVariantMap arguments)
{
    if (!collectionEditingAvailable(containerId))
        throw std::runtime_error("unsupported_extension");
    if (operation != QStringLiteral("collectionInfo") && operation != QStringLiteral("collectionRemove")
        && operation != QStringLiteral("collectionMove"))
        throw std::runtime_error("unsupported_extension");
    arguments.insert(QStringLiteral("containerId"), rawId(containerId));
    co_return co_await m_registry->callExtension(accountOf(containerId), QStringLiteral("spool.collection-editing"),
        std::move(operation), std::move(arguments), QStringLiteral("collection-editor"));
}

QCoro::Task<PagedMovieItems> SourceHub::collectionEntries(QString containerId, std::optional<QString> cursor)
{
    if (!collectionEditingAvailable(containerId))
        throw std::runtime_error("unsupported_extension");
    const QString account = accountOf(containerId);
    QVariantMap args { { QStringLiteral("containerId"), rawId(containerId) }, { QStringLiteral("limit"), 50 } };
    if (cursor)
        args.insert(QStringLiteral("cursor"), *cursor);
    QPointer<SourceHub> guard(this);
    auto page = co_await m_registry->callExtensionMediaPage(account, QStringLiteral("spool.collection-editing"),
        QStringLiteral("collectionEntries"), std::move(args), 50, QStringLiteral("collection-editor"));
    if (!guard)
        throw std::runtime_error("cancelled");
    PagedMovieItems result;
    result.items = scopedItems(std::move(page.items), account);
    result.nextCursor = std::move(page.cursor);
    result.exhausted = page.exhausted;
    result.limit = 50;
    co_return result;
}

void SourceHub::cancelCollection(const QString& containerId)
{
    m_registry->cancelSourceScope(accountOf(containerId), QStringLiteral("collection-editor"));
}

void SourceHub::collectionChanged(const QString& containerId)
{
    emit contentChanged(containerId);
}

void SourceHub::setPlaybackPreferences(
    qint64 manualMaxBitrate, bool unlimitedLocalNetwork, bool preferRemux, int maxHeight)
{
    m_preferences = { { QStringLiteral("preferredMaxBitrate"), manualMaxBitrate },
        { QStringLiteral("unlimitedLocalNetwork"), unlimitedLocalNetwork },
        { QStringLiteral("preferRemux"), preferRemux }, { QStringLiteral("preferredMaxHeight"), maxHeight } };
    pushPlaybackContext();
    emit streamingQualityChanged();
}

void SourceHub::setOverride(qint64 bitrate, int height)
{
    m_bitrate = bitrate;
    m_height = height;
    pushPlaybackContext();
}

void SourceHub::setPlaybackQueue(std::vector<ReportingQueueEntry> items, int index)
{
    if (items != m_reportingQueue) {
        m_reportingQueue = std::move(items);
        QHash<QString, QVariantList> accountItems;
        m_queueLocations.clear();
        m_queueLocations.reserve(m_reportingQueue.size());
        for (const auto& item : m_reportingQueue) {
            QString account = accountOf(item.itemId);
            // A stopped/restarting source still owns its queued media.
            if (account.isEmpty()) {
                for (const auto& saved : m_registry->accountList()) {
                    if (item.itemId.startsWith(prefixOf(saved.id) + QLatin1Char(':'))) {
                        account = saved.id;
                        break;
                    }
                }
            }
            if (account.isEmpty()) {
                m_queueLocations.emplace_back(QString(), -1);
                continue;
            }
            auto& rows = accountItems[account];
            m_queueLocations.emplace_back(account, static_cast<int>(rows.size()));
            QVariantMap row { { QStringLiteral("itemId"), rawId(item.itemId) },
                { QStringLiteral("mediaType"), item.audio ? QStringLiteral("audio") : QStringLiteral("video") } };
            if (!item.entryId.isEmpty())
                row.insert(QStringLiteral("entryId"), item.entryId);
            rows.append(row);
        }
        // Keep an empty revision for accounts whose last entry was removed.
        for (auto it = m_queueSnapshots.cbegin(); it != m_queueSnapshots.cend(); ++it) {
            if (!accountItems.contains(it.key()))
                accountItems.insert(it.key(), {});
        }
        for (auto it = accountItems.cbegin(); it != accountItems.cend(); ++it) {
            auto& snapshot = m_queueSnapshots[it.key()];
            if (snapshot.isEmpty() || snapshot.value(QStringLiteral("items")).toList() != it.value()) {
                snapshot = { { QStringLiteral("revision"), QString::number(++m_queueRevision) },
                    { QStringLiteral("items"), it.value() } };
            }
        }
    }
    m_queueIndexes.clear();
    if (index >= 0 && index < static_cast<int>(m_queueLocations.size())) {
        const auto& [account, localIndex] = m_queueLocations[static_cast<size_t>(index)];
        if (!account.isEmpty())
            m_queueIndexes.insert(account, localIndex);
    }
    for (const Entry& entry : std::as_const(m_entries)) {
        if (auto *portable = qobject_cast<PortableProvider *>(entry.provider.data()))
            portable->setPlaybackQueueContext(
                m_queueSnapshots.value(entry.accountId), m_queueIndexes.value(entry.accountId, -1));
    }
}

void SourceHub::pushPlaybackContext()
{
    // The viewer's pick in the player wins over the standing preference.
    QVariantMap context = m_preferences;
    context.insert(QStringLiteral("maxBitrate"), m_bitrate);
    context.insert(QStringLiteral("maxHeight"), m_height);
    context.insert(QStringLiteral("videoCodecs"), m_videoCodecs);
    context.insert(QStringLiteral("restrictVideoCodecs"), m_restrictVideoCodecs);
    for (const Entry& entry : std::as_const(m_entries)) {
        if (auto *portable = qobject_cast<PortableProvider *>(entry.provider.data())) {
            context.insert(QStringLiteral("measuredBitrate"), entry.measuredBitrate);
            context.insert(QStringLiteral("parallelRequests"), entry.parallelRequests);
            portable->setPlaybackContext(context);
            portable->setPlaybackQueueContext(
                m_queueSnapshots.value(entry.accountId), m_queueIndexes.value(entry.accountId, -1));
        }
    }
}

void SourceHub::cancelSpeedTest()
{
    m_speedTestTimer.stop();
    if (m_speedTestAccount.isEmpty())
        return;
    const QString account = std::exchange(m_speedTestAccount, {});
    ++m_speedTestGeneration;
    auto entry = m_entries.find(prefixOf(account));
    if (entry != m_entries.end())
        entry->speedState = Entry::SpeedState::Pending;
    m_registry->cancelSourceScope(account, QStringLiteral("speed-test"));
}

void SourceHub::setPlaybackActive(bool active)
{
    if (m_playbackActive == active)
        return;
    m_playbackActive = active;
    // A started bounded probe is allowed to finish; cancelling it here made
    // every quick Play leave Auto permanently without a measurement.
    if (active)
        m_speedTestTimer.stop();
    else if (m_speedTestAccount.isEmpty())
        m_speedTestTimer.start(5000);
    emit streamingQualityChanged();
}

void SourceHub::refreshSpeedTests()
{
    cancelSpeedTest();
    for (Entry& entry : m_entries)
        entry.speedState = Entry::SpeedState::Pending;
    m_explicitSpeedTest = true;
    m_speedTestTimer.start(0);
    emit streamingQualityChanged();
}

void SourceHub::startNextSpeedTest()
{
    if ((m_playbackActive && !m_explicitSpeedTest) || !m_speedTestAccount.isEmpty())
        return;
    for (Provider *provider : sources()) {
        if (!provider->capabilities().testFlag(Provider::SpeedTest))
            continue;
        const QString account = provider->id();
        Entry& entry = m_entries[prefixOf(account)];
        if (entry.speedState != Entry::SpeedState::Pending)
            continue;
        entry.speedState = Entry::SpeedState::Running;
        m_speedTestAccount = account;
        const quint64 generation = ++m_speedTestGeneration;
        emit streamingQualityChanged();
        const auto finish = [this, account, generation](const QVariantMap& result) {
            if (generation != m_speedTestGeneration)
                return;
            m_speedTestAccount.clear();
            auto entry = m_entries.find(prefixOf(account));
            if (entry != m_entries.end()) {
                const qint64 bitrate = result.value(QStringLiteral("bitrate")).toLongLong();
                const int lanes = result.value(QStringLiteral("parallelRequests")).toInt();
                const bool valid
                    = bitrate >= 1'000'000 && bitrate <= 1'000'000'000 && (lanes == 1 || lanes == 2 || lanes == 4);
                entry->speedState = valid ? Entry::SpeedState::Complete : Entry::SpeedState::Failed;
                if (valid) {
                    entry->measuredBitrate = bitrate;
                    entry->parallelRequests = lanes;
                    pushPlaybackContext();
                }
            }
            emit streamingQualityChanged();
            m_speedTestTimer.start(0);
        };
        Async::runScoped(
            this, m_registry->callSource(account, QStringLiteral("speedTest"), {}, QStringLiteral("speed-test")),
            finish, [finish](const std::exception_ptr&) { finish({}); }, "provider speed test");
        return;
    }
    m_explicitSpeedTest = false;
}

QString SourceHub::speedDescription(const Entry& entry) const
{
    QString detail;
    if (entry.measuredBitrate > 0)
        detail = QStringLiteral("Measured limit: %1 · %2 connection(s)")
                     .arg(formatBitrate(entry.measuredBitrate))
                     .arg(entry.parallelRequests);
    if (entry.speedState == Entry::SpeedState::Running)
        return detail.isEmpty() ? QStringLiteral("Measuring connection speed…")
                                : detail + QStringLiteral(" · Measuring…");
    if (entry.speedState == Entry::SpeedState::Pending && m_playbackActive)
        return detail.isEmpty() ? QStringLiteral("Speed test deferred until playback stops") : detail;
    if (entry.speedState == Entry::SpeedState::Pending)
        return detail.isEmpty() ? QStringLiteral("Waiting to measure connection speed…") : detail;
    if (entry.speedState == Entry::SpeedState::Failed)
        return detail.isEmpty() ? QStringLiteral("Connection speed unavailable")
                                : detail + QStringLiteral(" · Last test failed");
    return detail;
}

QString SourceHub::speedTestDescription() const
{
    QStringList descriptions;
    for (Provider *provider : sources()) {
        if (provider->capabilities().testFlag(Provider::SpeedTest))
            descriptions.append(provider->displayName() + QStringLiteral(": ")
                + speedDescription(m_entries.value(prefixOf(provider->id()))));
    }
    return descriptions.join(QLatin1Char('\n'));
}

QString SourceHub::autoDescription() const
{
    const auto entry = m_entries.constFind(prefixOf(m_playbackAccount));
    QStringList details;
    const qint64 preferred = m_preferences.value(QStringLiteral("preferredMaxBitrate")).toLongLong();
    if (preferred > 0)
        details.append(QStringLiteral("Settings limit: %1").arg(formatBitrate(preferred)));
    if (entry != m_entries.cend() && entry->provider && entry->provider->capabilities().testFlag(Provider::SpeedTest))
        details.append(speedDescription(*entry));
    if (details.isEmpty())
        return QStringLiteral("Original quality");
    return details.join(QStringLiteral(" · "));
}

MovieItem SourceHub::scopedItem(MovieItem item, const QString& accountId) const
{
    item.id = scoped(accountId, item.id);
    item.seriesId = scoped(accountId, item.seriesId);
    item.seasonId = scoped(accountId, item.seasonId);
    item.albumId = scoped(accountId, item.albumId);
    item.backdropItemId = scoped(accountId, item.backdropItemId);
    item.thumbItemId = scoped(accountId, item.thumbItemId);
    for (PersonItem& person : item.people)
        person.id = scoped(accountId, person.id);
    return item;
}

std::vector<MovieItem> SourceHub::scopedItems(std::vector<MovieItem> items, const QString& accountId) const
{
    for (MovieItem& item : items)
        item = scopedItem(std::move(item), accountId);
    return items;
}

template <typename Fetch> QCoro::Task<std::vector<MovieItem>> SourceHub::gather(Fetch fetch, int limit, bool searchOnly)
{
    // Tasks start eagerly, so every account is asked before any is awaited.
    std::vector<std::pair<QString, QCoro::Task<std::vector<MovieItem>>>> pending;
    for (Provider *provider : sources()) {
        if (searchOnly && !provider->search())
            continue;
        pending.emplace_back(provider->id(), fetch(provider));
    }
    std::vector<std::vector<MovieItem>> lists;
    for (auto& [accountId, task] : pending) {
        try {
            lists.push_back(scopedItems(co_await std::move(task), accountId));
        } catch (const std::exception& error) {
            qWarning() << "hub:" << accountId.left(kPrefix) << "left out of a merged list:" << error.what();
        }
    }
    co_return interleave(std::move(lists), limit);
}

bool SourceHub::signedIn() const
{
    return !sources().empty();
}

QString SourceHub::libraryScopeKey() const
{
    QStringList keys;
    for (Provider *provider : sources())
        keys.append(prefixOf(provider->id()));
    return keys.join(QLatin1Char('+'));
}

QCoro::Task<PagedMovieItems> SourceHub::fetchBrowsePage(
    BrowseDescriptor descriptor, int startIndex, int limit, QVariantMap queryOptions, std::optional<QString> cursor)
{
    // A genre or studio link has no ID of its own; it belongs to the account
    // whose item it was opened from.
    QString account = accountOf(descriptor.id.isEmpty() ? descriptor.seriesId : descriptor.id);
    if (account.isEmpty())
        account = m_lastDetailsAccount;
    Provider *provider = source(account);
    if (!provider)
        co_return PagedMovieItems { {}, 0, startIndex, limit };
    descriptor.id = rawId(descriptor.id);
    descriptor.seriesId = rawId(descriptor.seriesId);
    descriptor.seasonId = rawId(descriptor.seasonId);
    PagedMovieItems page = co_await provider->catalog()->fetchBrowsePage(
        descriptor, startIndex, limit, std::move(queryOptions), std::move(cursor));
    page.items = scopedItems(std::move(page.items), account);
    co_return page;
}

QCoro::Task<MovieItem> SourceHub::fetchItemDetails(QString itemId)
{
    const QString account = accountOf(itemId);
    Provider *provider = source(account);
    if (!provider)
        throw std::runtime_error("source_unavailable");
    m_lastDetailsAccount = account;
    co_return scopedItem(co_await provider->catalog()->fetchItemDetails(rawId(itemId)), account);
}

QCoro::Task<std::vector<MovieItem>> SourceHub::fetchSeasons(QString seriesId)
{
    const QString account = accountOf(seriesId);
    Provider *provider = source(account);
    if (!provider)
        co_return {};
    co_return scopedItems(co_await provider->catalog()->fetchSeasons(rawId(seriesId)), account);
}

QCoro::Task<std::vector<MovieItem>> SourceHub::fetchEpisodes(QString seriesId, QString seasonId)
{
    const QString account = accountOf(seriesId);
    Provider *provider = source(account);
    if (!provider)
        co_return {};
    co_return scopedItems(co_await provider->catalog()->fetchEpisodes(rawId(seriesId), rawId(seasonId)), account);
}

QCoro::Task<std::vector<MovieItem>> SourceHub::fetchResumeItems(int limit)
{
    return gather([limit](Provider *p) { return p->catalog()->fetchResumeItems(limit); }, limit);
}

QCoro::Task<std::vector<MovieItem>> SourceHub::fetchNextUpEpisodes(int limit)
{
    return gather([limit](Provider *p) { return p->catalog()->fetchNextUpEpisodes(limit); }, limit);
}

QCoro::Task<std::vector<MovieItem>> SourceHub::fetchLatestItems(QString parentId, int limit)
{
    if (parentId.isEmpty())
        co_return co_await gather([limit](Provider *p) { return p->catalog()->fetchLatestItems({}, limit); }, limit);
    const QString account = accountOf(parentId);
    Provider *provider = source(account);
    if (!provider)
        co_return {};
    co_return scopedItems(co_await provider->catalog()->fetchLatestItems(rawId(parentId), limit), account);
}

QCoro::Task<std::vector<MovieItem>> SourceHub::fetchSimilarItems(QString itemId, int limit)
{
    const QString account = accountOf(itemId);
    Provider *provider = source(account);
    if (!provider)
        co_return {};
    co_return scopedItems(co_await provider->catalog()->fetchSimilarItems(rawId(itemId), limit), account);
}

QCoro::Task<PersonCredits> SourceHub::fetchItemsByPerson(QString personId, int maximumItems)
{
    const QString account = accountOf(personId);
    Provider *provider = source(account);
    if (!provider)
        co_return PersonCredits {};
    PersonCredits credits = co_await provider->catalog()->fetchItemsByPerson(rawId(personId), maximumItems);
    credits.items = scopedItems(std::move(credits.items), account);
    credits.relatedSeries = scopedItems(std::move(credits.relatedSeries), account);
    co_return credits;
}

QCoro::Task<std::vector<LibraryItem>> SourceHub::fetchLibraries()
{
    std::vector<std::pair<QString, QCoro::Task<std::vector<LibraryItem>>>> pending;
    for (Provider *provider : sources())
        pending.emplace_back(provider->id(), provider->catalog()->fetchLibraries());
    std::vector<LibraryItem> libraries;
    for (auto& [accountId, task] : pending) {
        try {
            for (LibraryItem library : co_await std::move(task)) {
                library.id = scoped(accountId, library.id);
                libraries.push_back(std::move(library));
            }
        } catch (const std::exception& error) {
            qWarning() << "hub: libraries unavailable from" << accountId.left(kPrefix) << error.what();
        }
    }
    co_return libraries;
}

QCoro::Task<QVariantMap> SourceHub::fetchLibraryFilterOptions(QString libraryId, QString collectionType)
{
    Provider *provider = owner(libraryId);
    if (!provider)
        co_return QVariantMap {};
    co_return co_await provider->catalog()->fetchLibraryFilterOptions(rawId(libraryId), collectionType);
}

QCoro::Task<std::vector<MovieItem>> SourceHub::fetchItemsByIds(QStringList itemIds)
{
    QHash<QString, QStringList> byAccount;
    for (const QString& id : std::as_const(itemIds))
        byAccount[accountOf(id)].append(rawId(id));
    std::vector<std::pair<QString, QCoro::Task<std::vector<MovieItem>>>> pending;
    for (auto it = byAccount.cbegin(); it != byAccount.cend(); ++it) {
        if (Provider *provider = source(it.key()))
            pending.emplace_back(it.key(), provider->catalog()->fetchItemsByIds(it.value()));
    }
    QHash<QString, MovieItem> found;
    for (auto& [accountId, task] : pending) {
        try {
            for (MovieItem& item : scopedItems(co_await std::move(task), accountId)) {
                const QString id = item.id;
                found.insert(id, std::move(item));
            }
        } catch (const std::exception& error) {
            qWarning() << "hub:" << accountId.left(kPrefix) << "items unavailable:" << error.what();
        }
    }
    std::vector<MovieItem> ordered;
    ordered.reserve(itemIds.size());
    for (const QString& id : std::as_const(itemIds)) {
        const auto it = found.constFind(id);
        if (it != found.cend())
            ordered.push_back(*it);
    }
    co_return ordered;
}

QCoro::Task<std::vector<SourceHub::SearchTarget>> SourceHub::searchPlan()
{
    // Search the same viewers as Home. Saved alternate profiles are not a
    // permission union: a restricted viewer must not search an adult's libraries.
    std::vector<SearchTarget> plan;
    const auto& accounts = m_registry->accountList();
    for (const Entry& entry : std::as_const(m_entries)) {
        if (!entry.browse || !entry.provider || !entry.provider->search())
            continue;
        const auto account
            = std::find_if(accounts.begin(), accounts.end(), [&](const auto& a) { return a.id == entry.accountId; });
        const QString server = account == accounts.end() || account->group.isEmpty()
            ? entry.accountId
            : account->module + QLatin1Char('/') + account->group;
        plan.push_back({ entry.accountId, server });
    }
    std::sort(plan.begin(), plan.end(), [](const SearchTarget& a, const SearchTarget& b) {
        return std::tie(a.server, a.accountId) < std::tie(b.server, b.accountId);
    });
    co_return plan;
}

QCoro::Task<void> SourceHub::searchProgressively(QString searchTerm, int limit, SearchUpdate update)
{
    // Whatever the last query still has in flight is no longer wanted.
    const quint64 serial = ++m_searchSerial;
    for (const Entry& entry : std::as_const(m_entries))
        m_registry->cancelSourceScope(entry.accountId, QStringLiteral("search"));
    auto run = std::make_shared<SearchRun>();
    run->plan = co_await searchPlan();
    if (serial != m_searchSerial)
        co_return;
    run->found.resize(run->plan.size());
    run->query = folded(searchTerm);
    run->limit = limit;
    run->serial = serial;
    run->update = std::move(update);
    if (run->plan.empty()) {
        run->update({});
        co_return;
    }
    // Every account is asked before any answer is awaited, and each answer
    // is shown as it lands rather than when the slowest server is done.
    std::vector<QCoro::Task<void>> pending;
    for (size_t index = 0; index < run->plan.size(); ++index)
        pending.push_back(searchOne(run, index, searchTerm));
    for (auto& task : pending)
        co_await std::move(task);
}

QCoro::Task<void> SourceHub::searchOne(std::shared_ptr<SearchRun> run, size_t index, QString searchTerm)
{
    const QString accountId = run->plan[index].accountId;
    Provider *provider = source(accountId);
    if (!provider || !provider->search())
        co_return;
    try {
        std::vector<MovieItem> items = co_await provider->search()->searchItems(searchTerm, run->limit);
        if (run->serial != m_searchSerial)
            co_return;
        run->found[index] = scopedItems(std::move(items), accountId);
        run->update(run->merged());
    } catch (const std::exception& error) {
        if (run->serial == m_searchSerial)
            qWarning() << "hub:" << accountId.left(kPrefix) << "left out of search:" << error.what();
    }
}

QCoro::Task<std::vector<MovieItem>> SourceHub::searchItems(QString searchTerm, int limit)
{
    std::vector<MovieItem> latest;
    co_await searchProgressively(
        std::move(searchTerm), limit, [&latest](std::vector<MovieItem> items) { latest = std::move(items); });
    co_return latest;
}

QCoro::Task<std::vector<MovieItem>> SourceHub::fetchSearchSuggestions(int limit)
{
    return gather([limit](Provider *p) { return p->search()->fetchSearchSuggestions(limit); }, limit, true);
}

QCoro::Task<void> SourceHub::setItemFavorite(QString itemId, bool favorite)
{
    if (Provider *provider = owner(itemId); provider && provider->itemState())
        co_await provider->itemState()->setItemFavorite(rawId(itemId), favorite);
}

QCoro::Task<void> SourceHub::setItemPlayed(QString itemId, bool played)
{
    if (Provider *provider = owner(itemId); provider && provider->itemState())
        co_await provider->itemState()->setItemPlayed(rawId(itemId), played);
}

QCoro::Task<void> SourceHub::setItemPlaybackPosition(QString itemId, qint64 positionTicks)
{
    if (Provider *provider = owner(itemId); provider && provider->itemState())
        co_await provider->itemState()->setItemPlaybackPosition(rawId(itemId), positionTicks);
}

QString SourceHub::imageUrl(const ImageRequest& request) const
{
    Provider *provider = owner(request.itemId);
    if (!provider)
        return {};
    ImageRequest local = request;
    local.itemId = rawId(request.itemId);
    return provider->artwork()->imageUrl(local);
}

ArtworkSource::ImageResource SourceHub::resolveImage(const QUrl& url) const
{
    if (url.scheme() != QLatin1String("spool-artwork"))
        return ArtworkSource::resolveImage(url);
    const QString host = url.host();
    if (!host.startsWith(QLatin1String("account-")))
        return {};
    const auto entry = m_entries.constFind(host.mid(8));
    if (entry == m_entries.cend() || !entry->provider)
        return {};
    if (url.path().startsWith(QLatin1String("/preview/"))) {
        const auto preview = entry->playbackPreviews.constFind(url.path().mid(9));
        bool ok = false;
        const int index = QUrlQuery(url).queryItemValue(QStringLiteral("index")).toInt(&ok);
        if (preview == entry->playbackPreviews.cend() || !ok || index < 0)
            return {};
        QString resolved = preview->urlTemplate;
        resolved.replace(QLatin1String("{index}"), QString::number(index));
        const QUrl resource(resolved, QUrl::StrictMode);
        const auto *playback = entry->provider->playback();
        if (!playback || !m_registry->accountOriginAllowed(entry->accountId, resource)
            || resource.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment)
                != playback->mediaOrigin())
            return {};
        return { resource, preview->headers };
    }
    if (url.path().startsWith(QLatin1String("/remote/"))) {
        const QString target = QString::fromUtf8(QByteArray::fromBase64(
            url.path().mid(8).toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors));
        const auto preview = entry->remotePreviews.constFind(target);
        bool ok = false;
        const QUrlQuery query(url);
        const int index = query.queryItemValue(QStringLiteral("index")).toInt(&ok);
        if (preview == entry->remotePreviews.cend() || !ok || index < 0
            || query.queryItemValue(QStringLiteral("revision")) != preview->revision)
            return {};
        QString resolved = preview->urlTemplate;
        resolved.replace(QLatin1String("{index}"), QString::number(index));
        const QUrl resource(resolved);
        if (!m_registry->accountOriginAllowed(entry->accountId, resource))
            return {};
        return { resource, preview->headers };
    }
    return {};
}

} // namespace Spool
