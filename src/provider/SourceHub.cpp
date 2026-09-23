#include "SourceHub.h"

#include "../common/AsyncTask.h"
#include "PortableProvider.h"
#include "ProviderRegistry.h"

#include <QDebug>

#include <algorithm>

namespace JellyfinNative {

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

    // Everything found so far, each item once per server, ranked by how well
    // its title matches and then by each server's own order, so equally good
    // answers from different servers interleave.
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
        std::vector<MovieItem> items;
        items.reserve(std::min(ranked.size(), size_t(limit)));
        for (const Ranked& entry : ranked) {
            if (items.size() >= size_t(limit))
                break;
            items.push_back(*entry.item);
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
    QString trickplayTileUrl(const QString& itemId, int width, int tileIndex) const override
    {
        PlaybackSource *playback = sourceFor(itemId);
        return playback ? playback->trickplayTileUrl(rawId(itemId), width, tileIndex) : QString();
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
        }
        item.id = rawId(item.id);
        item.seriesId = rawId(item.seriesId);
        item.seasonId = rawId(item.seasonId);
        item.albumId = rawId(item.albumId);
        PlaybackSession session = co_await playback->resolvePlayback(std::move(item), forceTranscode);
        session.itemId = m_hub->scoped(account, session.itemId);
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
    connect(registry, &ProviderRegistry::restoredChanged, this, [this] {
        // Nothing to wait for: announce the empty state so the shell moves on.
        if (m_registry->restored()
            && std::none_of(m_registry->accountList().begin(), m_registry->accountList().end(),
                [](const ProviderAccount& account) { return account.enabled; }))
            m_settled.start(0);
    });
}

SourceHub::~SourceHub() = default;

QString SourceHub::displayName() const
{
    return QStringLiteral("Spool");
}

PlaybackSource *SourceHub::playback()
{
    return m_playback;
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
    connect(provider, &Provider::contentChanged, this, [this, accountId](const QString& itemId) {
        if (itemId.isEmpty())
            m_access.remove(accountId);
        emit contentChanged(itemId.isEmpty() ? QString() : scoped(accountId, itemId));
    });
    connect(provider, &Provider::sourceEvent, this, [this, accountId](const QString& type, const QVariantMap& payload) {
        emit accountEvent(accountId, type, payload);
    });
    connect(provider, &Provider::errorOccurred, this, &Provider::errorOccurred);
    connect(provider, &Provider::toastRequested, this, &Provider::toastRequested);
    pushPlaybackContext();
    // A set-aside account joining for search changes nothing on screen.
    if (browse)
        refresh();
}

void SourceHub::removeSource(const QString& accountId)
{
    const Entry entry = m_entries.take(prefixOf(accountId));
    m_access.remove(accountId);
    if (entry.browse)
        refresh();
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
        changed |= entry.browse != browse;
        entry.browse = browse;
    }
    if (changed)
        refresh();
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

QVariantList SourceHub::itemActions(const QString& itemId, const QString& itemType) const
{
    const QString account = accountOf(itemId);
    const auto& accounts = m_registry->accountList();
    const auto owner = std::find_if(accounts.begin(), accounts.end(), [&](const auto& a) { return a.id == account; });
    const ProviderModule *module = owner == accounts.end() ? nullptr : m_registry->module(owner->module);
    QVariantList actions;
    if (!module)
        return actions;
    for (const QVariant& value : module->manifest.actions) {
        const QVariantMap action = value.toMap();
        const QStringList types = action.value(QStringLiteral("types")).toStringList();
        if (types.isEmpty() || types.contains(itemType))
            actions.append(action);
    }
    return actions;
}

void SourceHub::runItemAction(const QString& actionId, const QString& itemId, const QString& itemType)
{
    const QString account = accountOf(itemId);
    const auto run = [](SourceHub *self, QString account, QVariantMap args) -> QCoro::Task<void> {
        QPointer<SourceHub> guard(self);
        QVariantMap result = co_await self->call(account, QStringLiteral("runItemAction"), args);
        if (guard && result.contains(QStringLiteral("pick"))) {
            const QVariantMap choice
                = co_await self->m_registry->pick(account, result.value(QStringLiteral("pick")).toMap());
            if (!guard || choice.isEmpty())
                co_return;
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
        run(this, account,
            { { QStringLiteral("action"), actionId }, { QStringLiteral("itemId"), rawId(itemId) },
                { QStringLiteral("itemType"), itemType } }),
        [] {}, [this](const std::exception_ptr&) { emit toastRequested(QStringLiteral("That didn't work")); },
        "item action");
}

void SourceHub::setPlaybackPreferences(
    qint64 manualMaxBitrate, bool unlimitedLocalNetwork, bool preferRemux, int maxHeight)
{
    m_preferences = { { QStringLiteral("preferredMaxBitrate"), manualMaxBitrate },
        { QStringLiteral("unlimitedLocalNetwork"), unlimitedLocalNetwork },
        { QStringLiteral("preferRemux"), preferRemux }, { QStringLiteral("preferredMaxHeight"), maxHeight } };
    pushPlaybackContext();
}

void SourceHub::setOverride(qint64 bitrate, int height)
{
    m_bitrate = bitrate;
    m_height = height;
    pushPlaybackContext();
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
        if (auto *portable = qobject_cast<PortableProvider *>(entry.provider.data()))
            portable->setPlaybackContext(context);
    }
}

MovieItem SourceHub::scopedItem(MovieItem item, const QString& accountId) const
{
    item.id = scoped(accountId, item.id);
    item.seriesId = scoped(accountId, item.seriesId);
    item.seasonId = scoped(accountId, item.seasonId);
    item.albumId = scoped(accountId, item.albumId);
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
    BrowseDescriptor descriptor, int startIndex, int limit, QVariantMap queryOptions)
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
    PagedMovieItems page = co_await provider->catalog()->fetchBrowsePage(descriptor, startIndex, limit, queryOptions);
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
    std::vector<std::pair<Provider *, QCoro::Task<std::vector<LibraryItem>>>> pending;
    for (Provider *provider : sources())
        pending.emplace_back(provider, provider->catalog()->fetchLibraries());
    std::vector<LibraryItem> libraries;
    QHash<QString, int> names;
    for (auto& [provider, task] : pending) {
        try {
            for (LibraryItem library : co_await std::move(task)) {
                library.id = scoped(provider->id(), library.id);
                names[library.name.toCaseFolded()] += 1;
                libraries.push_back(std::move(library));
            }
        } catch (const std::exception& error) {
            qWarning() << "hub: libraries unavailable from" << provider->displayName() << error.what();
        }
    }
    // Two "Movies" rows from two servers read as one; say whose each is.
    const auto& accounts = m_registry->accountList();
    for (LibraryItem& library : libraries) {
        if (names.value(library.name.toCaseFolded()) > 1) {
            const QString account = accountOf(library.id);
            const auto found
                = std::find_if(accounts.begin(), accounts.end(), [&](const auto& a) { return a.id == account; });
            if (found != accounts.end())
                library.name += QStringLiteral(" · ") + found->label;
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
            for (MovieItem& item : scopedItems(co_await std::move(task), accountId))
                found.insert(item.id, std::move(item));
        } catch (const std::exception& error) {
            qWarning() << "hub:" << accountId.left(kPrefix) << "items unavailable:" << error.what();
        }
    }
    std::vector<MovieItem> ordered;
    for (const QString& id : std::as_const(itemIds)) {
        if (found.contains(id))
            ordered.push_back(found.value(id));
    }
    co_return ordered;
}

void SourceHub::prepareSearch()
{
    m_registry->startSetAside();
}

QCoro::Task<std::optional<QSet<QString>>> SourceHub::accessOf(QString accountId)
{
    if (const auto cached = m_access.constFind(accountId); cached != m_access.cend())
        co_return *cached;
    Provider *provider = source(accountId);
    if (!provider)
        co_return std::nullopt;
    try {
        QSet<QString> ids;
        for (const LibraryItem& library : co_await provider->catalog()->fetchLibraries())
            ids.insert(library.id);
        m_access.insert(accountId, ids);
        co_return ids;
    } catch (const std::exception& error) {
        qWarning() << "hub:" << accountId.left(kPrefix) << "libraries unknown for search:" << error.what();
    }
    co_return std::nullopt;
}

QCoro::Task<std::vector<SourceHub::SearchTarget>> SourceHub::searchPlan()
{
    struct Candidate {
        QString accountId;
        bool active = false;
        qint64 lastUsed = 0;
    };
    // Accounts are grouped by provider and server; a set-aside account is
    // only considered while another user of its server is in use.
    QHash<QString, std::vector<Candidate>> servers;
    QStringList order;
    const auto& accounts = m_registry->accountList();
    for (const Entry& entry : std::as_const(m_entries)) {
        if (!entry.provider || !entry.provider->search())
            continue;
        const auto account
            = std::find_if(accounts.begin(), accounts.end(), [&](const auto& a) { return a.id == entry.accountId; });
        const QString server = account == accounts.end() || account->group.isEmpty()
            ? entry.accountId
            : account->module + QLatin1Char('/') + account->group;
        if (!servers.contains(server))
            order.append(server);
        servers[server].push_back({ entry.accountId, entry.browse, account == accounts.end() ? 0 : account->lastUsed });
    }
    order.sort();

    std::vector<SearchTarget> plan;
    for (const QString& server : std::as_const(order)) {
        std::vector<Candidate> candidates = servers.value(server);
        if (std::none_of(candidates.begin(), candidates.end(), [](const Candidate& c) { return c.active; }))
            continue;
        if (candidates.size() == 1) {
            plan.push_back({ candidates.front().accountId, server });
            continue;
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return std::tie(b.active, b.lastUsed) < std::tie(a.active, a.lastUsed);
        });
        // Asked of every candidate at once; cached until the server changes.
        std::vector<QCoro::Task<std::optional<QSet<QString>>>> pending;
        for (const Candidate& candidate : candidates)
            pending.push_back(accessOf(candidate.accountId));
        std::vector<std::optional<QSet<QString>>> access;
        for (auto& task : pending)
            access.push_back(co_await std::move(task));

        QSet<QString> uncovered;
        for (const auto& libraries : access) {
            if (libraries)
                uncovered.unite(*libraries);
        }
        std::vector<bool> chosen(candidates.size(), false);
        // An account in use whose libraries could not be read still searches:
        // nothing can be said to cover it.
        for (size_t index = 0; index < candidates.size(); ++index)
            chosen[index] = candidates[index].active && !access[index];
        // Greedy cover: the account adding the most unseen libraries next,
        // the one in use first among equals.
        while (!uncovered.isEmpty()) {
            size_t best = candidates.size();
            qsizetype bestGain = 0;
            for (size_t index = 0; index < candidates.size(); ++index) {
                if (chosen[index] || !access[index])
                    continue;
                const qsizetype gain = QSet<QString>(*access[index]).intersect(uncovered).size();
                if (gain > bestGain) {
                    best = index;
                    bestGain = gain;
                }
            }
            if (best == candidates.size())
                break;
            chosen[best] = true;
            uncovered.subtract(*access[best]);
        }
        if (std::none_of(chosen.begin(), chosen.end(), [](bool c) { return c; }))
            chosen.front() = true;
        for (size_t index = 0; index < candidates.size(); ++index) {
            if (chosen[index])
                plan.push_back({ candidates[index].accountId, server });
        }
    }
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

} // namespace JellyfinNative
