#include "ProviderRegistry.h"

#include "../cache/DatabaseManager.h"
#include "../common/AsyncTask.h"
#include "../platform/CredentialStore.h"
#include "PortableProvider.h"
#include "ProviderCapabilityContract.h"
#include "ProviderExtensionData.h"
#include "ProviderUiContext.h"

#include <QCoroFuture>
#include <QCoroSignal>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <string_view>

namespace Spool {

namespace {
    const QString kAccountsKey = QStringLiteral("providers/accounts/2");

    bool requiresSignIn(const std::exception& error)
    {
        const std::string_view code(error.what());
        return code == "http_401" || code == "invalid_config" || code == "auth_required" || code == "invalid_token";
    }

    // One sentence for the profile that failed to open; provider codes are
    // grouped by what the viewer can do about them, never shown verbatim.
    QString activationError(const std::exception& error)
    {
        const std::string_view code(error.what());
        if (requiresSignIn(error))
            return QStringLiteral("Sign in again to use this profile.");
        if (code == "invalid_pin")
            return QStringLiteral("That PIN wasn't accepted. Try again.");
        if (code == "permission_denied")
            return QStringLiteral("This profile doesn't have access to this server.");
        if (code == "network_error" || code == "operation_timeout" || code == "server_unreachable")
            return QStringLiteral("Couldn't reach the server. Check the connection and try again.");
        return QStringLiteral("Couldn't open this profile. Try again.");
    }

    const QHash<QString, Provider::Capability>& capabilityNames()
    {
        static const QHash<QString, Provider::Capability> names {
            { QStringLiteral("search"), Provider::Search },
            { QStringLiteral("userState"), Provider::UserItemState },
            { QStringLiteral("reporting"), Provider::PlaybackReporting },
            { QStringLiteral("segments"), Provider::Segments },
            { QStringLiteral("groupPlayback"), Provider::GroupPlayback },
            { QStringLiteral("remoteControl"), Provider::RemoteControl },
            { QStringLiteral("streamQuality"), Provider::StreamQuality },
            { QStringLiteral("trickplay"), Provider::Trickplay },
            { QStringLiteral("speedTest"), Provider::SpeedTest },
            { QStringLiteral("downloads"), Provider::Downloads },
            { QStringLiteral("downloadTranscode"), Provider::DownloadTranscode },
        };
        return names;
    }

    Provider::Capabilities capabilitiesOf(const QVariantMap& capabilities)
    {
        Provider::Capabilities flags;
        for (auto it = capabilities.cbegin(); it != capabilities.cend(); ++it) {
            if (it.value().toBool())
                flags |= capabilityNames().value(it.key(), Provider::Capability {});
        }
        return flags;
    }

    QUrl originOf(const QUrl& url)
    {
        if (url.toString() == QStringLiteral("*"))
            return url;
        QUrl origin;
        origin.setScheme(url.scheme());
        origin.setHost(url.host());
        origin.setPort(url.port());
        return origin;
    }

    QUrl grantOrigin(const QUrl& url)
    {
        if (!url.isValid() || url.isRelative() || url.host().isEmpty() || !url.userInfo().isEmpty()
            || url.authority().contains(QLatin1Char('@')) || url.host().contains(QLatin1Char('*')) || url.port() == 0
            || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")))
            throw std::runtime_error("origin_denied");
        QUrl origin = originOf(url);
        if (origin.port() == (origin.scheme() == QStringLiteral("https") ? 443 : 80))
            origin.setPort(-1);
        return origin;
    }

    QJsonObject toJson(const ProviderAccount& account)
    {
        QJsonArray origins;
        for (const QUrl& origin : account.origins)
            origins.append(origin.toString());
        return { { QStringLiteral("id"), account.id }, { QStringLiteral("module"), account.module },
            { QStringLiteral("key"), account.key }, { QStringLiteral("group"), account.group },
            { QStringLiteral("label"), account.label }, { QStringLiteral("detail"), account.detail },
            { QStringLiteral("enabled"), account.enabled }, { QStringLiteral("origins"), origins },
            { QStringLiteral("lastUsed"), account.lastUsed },
            { QStringLiteral("activationFamily"), account.activationFamily },
            { QStringLiteral("activationIdentity"), account.activationIdentity } };
    }

    ProviderAccount fromJson(const QJsonObject& row)
    {
        ProviderAccount account;
        account.id = row.value(QStringLiteral("id")).toString();
        account.module = row.value(QStringLiteral("module")).toString();
        account.key = row.value(QStringLiteral("key")).toString();
        account.group = row.value(QStringLiteral("group")).toString();
        account.label = row.value(QStringLiteral("label")).toString();
        account.detail = row.value(QStringLiteral("detail")).toString();
        account.enabled = row.value(QStringLiteral("enabled")).toBool(true);
        for (const QJsonValue& origin : row.value(QStringLiteral("origins")).toArray())
            account.origins.append(QUrl(origin.toString()));
        account.lastUsed = row.value(QStringLiteral("lastUsed")).toInteger();
        account.activationFamily = row.value(QStringLiteral("activationFamily")).toString();
        account.activationIdentity = row.value(QStringLiteral("activationIdentity")).toString();
        return account;
    }
} // namespace

namespace {
    const QString activationCapability = QStringLiteral("accountActivation");

    void readActivation(ProviderAccount& candidate, const QVariantMap& description)
    {
        const QVariant value = description.value(QStringLiteral("activation"));
        if (!value.isValid()) {
            if (!candidate.activationFamily.isEmpty())
                throw std::runtime_error("activation_identity_changed");
            return;
        }
        const auto map = value.toMap();
        const QVariant family = map.value(QStringLiteral("familyId"));
        const QVariant identity = map.value(QStringLiteral("identityId"));
        if (value.metaType().id() != QMetaType::QVariantMap || family.metaType().id() != QMetaType::QString
            || identity.metaType().id() != QMetaType::QString || family.toString().isEmpty()
            || identity.toString().isEmpty() || family.toString().size() > 256 || identity.toString().size() > 256)
            throw std::runtime_error("invalid_activation");
        if ((!candidate.activationFamily.isEmpty() && candidate.activationFamily != family.toString())
            || (!candidate.activationIdentity.isEmpty() && candidate.activationIdentity != identity.toString()))
            throw std::runtime_error("activation_identity_changed");
        candidate.activationFamily = family.toString();
        candidate.activationIdentity = identity.toString();
    }

    QVariantMap activationOptions(const QVariant& value)
    {
        if (value.metaType().id() != QMetaType::QVariantMap)
            throw std::runtime_error("invalid_activation");
        const auto options = value.toMap();
        if (options.size() > 32)
            throw std::runtime_error("invalid_activation");
        for (auto it = options.cbegin(); it != options.cend(); ++it) {
            if (it.key().isEmpty() || it.key().size() > 128 || it.value().metaType().id() != QMetaType::Bool)
                throw std::runtime_error("invalid_activation");
        }
        return options;
    }
}

ProviderRegistry::ProviderRegistry(DatabaseManager *database, QObject *parent)
    : QObject(parent)
    , m_database(database)
{
    m_credentialPool.setMaxThreadCount(1);
}

ProviderRegistry::~ProviderRegistry()
{
    for (const QString& id : m_running.keys())
        stop(id);
}

void ProviderRegistry::setRuntimeEnvironment(QVariantMap device, ScriptRuntime::NetworkHooks hooks)
{
    m_device = std::move(device);
    m_hooks = std::move(hooks);
}

void ProviderRegistry::setInstallDirectory(const QString& path)
{
    m_installDirectory = path;
}

void ProviderRegistry::registerPackage(ProviderManifest manifest, QUrl root, bool bundled)
{
    const QString id = manifest.id;
    const auto existing = m_modules.constFind(id);
    if (existing != m_modules.cend()) {
        if (bundled || ProviderPackage::compareVersions(manifest.version, existing->manifest.version) <= 0)
            return;
    }
    ProviderModule module;
    module.overridesBundled = existing != m_modules.cend() && (existing->bundled || existing->overridesBundled);
    module.manifest = std::move(manifest);
    module.root = std::move(root);
    module.bundled = bundled;
    module.runtime = existing != m_modules.cend() ? existing->runtime : nullptr;
    m_modules.insert(id, std::move(module));
}

void ProviderRegistry::loadModules()
{
    QDirIterator bundled(QStringLiteral(":/providers"), QDir::Dirs | QDir::NoDotAndDotDot);
    while (bundled.hasNext()) {
        const QString path = bundled.next();
        QFile file(path + QStringLiteral("/manifest.json"));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        if (auto manifest = ProviderManifest::parse(file.readAll()))
            registerPackage(std::move(*manifest), QUrl(QStringLiteral("qrc") + path + QLatin1Char('/')), true);
    }
    // A build with no install directory runs only what it bundles.
    const auto installed = m_installDirectory.isEmpty() ? QMap<QString, QString> {}
                                                        : ProviderPackage::installedVersions(m_installDirectory);
    for (auto it = installed.cbegin(); it != installed.cend(); ++it) {
        QFile file(it.value() + QStringLiteral("/manifest.json"));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        QString error;
        if (auto manifest = ProviderManifest::parse(file.readAll(), &error))
            registerPackage(std::move(*manifest), QUrl::fromLocalFile(it.value() + QLatin1Char('/')), false);
        else
            qWarning("providers: skipping %s: %s", qPrintable(it.key()), qPrintable(error));
    }
    emit modulesChanged();
}

void ProviderRegistry::addNativeModule(ProviderManifest manifest, ProviderModule::NativeFactory factory, QUrl uiRoot)
{
    ProviderModule module;
    module.manifest = std::move(manifest);
    module.native = std::move(factory);
    module.root = std::move(uiRoot);
    module.bundled = true;
    m_modules.insert(module.manifest.id, std::move(module));
    emit modulesChanged();
}

const ProviderModule *ProviderRegistry::module(const QString& id) const
{
    const auto found = m_modules.constFind(id);
    return found == m_modules.cend() ? nullptr : &*found;
}

QStringList ProviderRegistry::moduleIds() const
{
    return m_modules.keys();
}

ProviderAccount *ProviderRegistry::account(const QString& id)
{
    const auto found = std::find_if(m_accounts.begin(), m_accounts.end(), [&](const auto& a) { return a.id == id; });
    return found == m_accounts.end() ? nullptr : &*found;
}

ScriptRuntime *ProviderRegistry::runtimeFor(ProviderModule& module)
{
    if (!module.runtime && !module.native) {
        const QUrl entry = module.file(module.manifest.entry);
        module.runtime = new ScriptRuntime(
            entry.isLocalFile() ? entry.toLocalFile() : entry.toString(), m_device, m_hooks, this, module.manifest.id);
        const QString id = module.manifest.id;
        connect(module.runtime, &ScriptRuntime::event, this, &ProviderRegistry::handleEvent);
        connect(module.runtime, &ScriptRuntime::interrupted, this, [this, id] { handleInterrupted(id); });
    }
    return module.runtime;
}

QCoro::Task<void> ProviderRegistry::restore()
{
    QPointer<ProviderRegistry> guard(this);
    const QString stored = co_await m_database->loadSettingAsync(kAccountsKey);
    if (!guard)
        co_return;
    const QJsonObject root = QJsonDocument::fromJson(stored.toUtf8()).object();
    m_activationOptions = root.value(QStringLiteral("activationOptions")).toObject().toVariantMap();
    m_startupChoices = root.value(QStringLiteral("startup")).toObject().toVariantMap();
    std::vector<ProviderAccount> accounts;
    for (const QJsonValue& row : root.value(QStringLiteral("accounts")).toArray()) {
        ProviderAccount account = fromJson(row.toObject());
        if (!QUuid(account.id).isNull() && m_modules.contains(account.module))
            accounts.push_back(std::move(account));
    }
    // Configuration holds credentials, so it lives in the platform's
    // credential store; the keychain can block, so it is read off-thread.
    struct RestoredAccounts {
        std::vector<ProviderAccount> accounts;
        QSet<QString> unavailableConfigurations;
    };
    auto restored = co_await Async::background(
        [accounts = std::move(accounts)]() mutable {
            RestoredAccounts result { std::move(accounts), {} };
            for (ProviderAccount& account : result.accounts) {
                const QJsonDocument configuration = QJsonDocument::fromJson(CredentialStore::load(account.id).toUtf8());
                // An empty object is valid provider configuration; only missing,
                // malformed, or non-object records are unavailable.
                if (configuration.isObject())
                    account.configuration = configuration.object().toVariantMap();
                else
                    result.unavailableConfigurations.insert(account.id);
            }
            return result;
        },
        &m_credentialPool);
    if (!guard)
        co_return;
    m_accounts = std::move(restored.accounts);
    for (const ProviderAccount& account : m_accounts) {
        if (restored.unavailableConfigurations.contains(account.id) && !module(account.module)->native) {
            m_unavailableConfigurations.insert(account.id);
            m_failedAccounts.insert(account.id);
        }
    }
    // One-time: carry over the Jellyfin sign-ins the native client kept, so
    // an upgrade signs nobody out.
    if (stored.isEmpty() && m_modules.contains(QStringLiteral("spool.jellyfin"))) {
        const QVariantList legacy = co_await m_database->loadLegacyAccountsAsync();
        if (!guard)
            co_return;
        QHash<QString, qint64> newestPerServer;
        for (const QVariant& value : legacy) {
            const QVariantMap row = value.toMap();
            const QString server = row.value(QStringLiteral("serverId")).toString();
            newestPerServer[server]
                = std::max(newestPerServer.value(server), row.value(QStringLiteral("lastUsed")).toLongLong());
        }
        for (const QVariant& value : legacy) {
            QVariantMap row = value.toMap();
            if (row.value(QStringLiteral("token")).toString().isEmpty())
                continue;
            ProviderAccount account;
            account.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            account.module = QStringLiteral("spool.jellyfin");
            account.group = row.value(QStringLiteral("serverId")).toString();
            account.key = row.value(QStringLiteral("userId")).toString() + QLatin1Char('@') + account.group;
            account.label = row.value(QStringLiteral("userName")).toString();
            account.detail = row.value(QStringLiteral("serverName")).toString();
            account.lastUsed = row.take(QStringLiteral("lastUsed")).toLongLong();
            account.enabled = account.lastUsed == newestPerServer.value(account.group);
            account.origins = { originOf(QUrl(row.value(QStringLiteral("server")).toString())) };
            account.configuration = row;
            m_accounts.push_back(std::move(account));
        }
        if (!m_accounts.empty())
            persist(true);
    }
    applyStartupChoices();
    m_restored = true;
    // Accounts first: the shell picks its first route when restored changes,
    // from bindings on the account list.
    emit accountsChanged();
    emit restoredChanged();
    QHash<QString, QStringList> modules;
    for (const ProviderAccount& account : m_accounts) {
        if (account.enabled)
            modules[account.module].append(account.id);
    }
    for (auto ids : modules) {
        std::stable_sort(ids.begin(), ids.end(),
            [this](const QString& a, const QString& b) { return account(a)->lastUsed > account(b)->lastUsed; });
        Async::runScoped(this, startRestored(ids), [] { }, [](const std::exception_ptr&) { }, "provider restore");
    }
}

QString ProviderRegistry::familyKey(const ProviderAccount& candidate) const
{
    // JSON encoding is injective even when opaque IDs contain punctuation.
    return QString::fromUtf8(
        QJsonDocument(QJsonArray { candidate.module, candidate.activationFamily }).toJson(QJsonDocument::Compact));
}

void ProviderRegistry::clearGrants(const QString& moduleId, const QString& family)
{
    for (auto it = m_familyEpochs.begin(); it != m_familyEpochs.end(); ++it) {
        const auto key = QJsonDocument::fromJson(it.key().toUtf8()).array();
        if (key.at(0).toString() == moduleId && (family.isEmpty() || key.at(1).toString() == family))
            ++it.value();
    }
    for (auto it = m_activationGrants.begin(); it != m_activationGrants.end();) {
        const auto key = QJsonDocument::fromJson(it.key().toUtf8()).array();
        if (key.at(0).toString() == moduleId && (family.isEmpty() || key.at(1).toString() == family))
            it = m_activationGrants.erase(it);
        else
            ++it;
    }
}

QString ProviderRegistry::profileSet(const ProviderAccount& candidate) const
{
    const bool family = !candidate.activationFamily.isEmpty();
    return QString::fromUtf8(
        QJsonDocument(QJsonArray { candidate.module, family ? QStringLiteral("family") : QStringLiteral("group"),
                          family                          ? candidate.activationFamily
                              : candidate.group.isEmpty() ? candidate.id
                                                          : candidate.group })
            .toJson(QJsonDocument::Compact));
}

bool ProviderRegistry::sameProfile(const ProviderAccount& a, const ProviderAccount& b) const
{
    return a.id == b.id
        || (a.module == b.module && !a.activationFamily.isEmpty() && a.activationFamily == b.activationFamily
            && a.activationIdentity == b.activationIdentity);
}

bool ProviderRegistry::startupDefault(const ProviderAccount& candidate) const
{
    const auto choice = m_startupChoices.value(profileSet(candidate)).toMap();
    if (choice.value(QStringLiteral("mode")).toString() != QStringLiteral("always"))
        return false;
    const QString pinned = choice.value(QStringLiteral("account")).toString();
    const auto found = std::find_if(
        m_accounts.cbegin(), m_accounts.cend(), [&](const ProviderAccount& entry) { return entry.id == pinned; });
    return found != m_accounts.cend() && sameProfile(*found, candidate);
}

// Startup choices decide which viewer each set opens before anything starts:
// a pinned profile replaces whoever was last selected, and a set that asks
// starts no one until a viewer is chosen.
void ProviderRegistry::applyStartupChoices()
{
    m_awaitingChoice.clear();
    for (auto it = m_startupChoices.begin(); it != m_startupChoices.end();) {
        const auto choice = it.value().toMap();
        const QString mode = choice.value(QStringLiteral("mode")).toString();
        const ProviderAccount *pinned = account(choice.value(QStringLiteral("account")).toString());
        const bool members = std::any_of(m_accounts.cbegin(), m_accounts.cend(),
            [&](const ProviderAccount& entry) { return profileSet(entry) == it.key(); });
        if (!members || (mode != QStringLiteral("ask") && mode != QStringLiteral("always"))
            || (mode == QStringLiteral("always") && (!pinned || profileSet(*pinned) != it.key()))) {
            it = m_startupChoices.erase(it);
            continue;
        }
        if (mode == QStringLiteral("ask"))
            m_awaitingChoice.insert(it.key());
        for (ProviderAccount& entry : m_accounts) {
            if (profileSet(entry) == it.key())
                entry.enabled = mode == QStringLiteral("always") && sameProfile(entry, *pinned);
        }
        ++it;
    }
    if (!m_startupChoices.isEmpty())
        persist();
}

QString ProviderRegistry::runtimeSourceId(const QString& sourceId) const
{
    return m_running.value(sourceId).runtimeId;
}

bool ProviderRegistry::lastUsedIdentity(const ProviderAccount& candidate) const
{
    // A person was last used when any of their servers was.
    qint64 latest = candidate.lastUsed;
    for (const auto& other : m_accounts) {
        if (sameProfile(other, candidate))
            latest = std::max(latest, other.lastUsed);
    }
    for (const auto& other : m_accounts) {
        if (other.module != candidate.module || sameProfile(other, candidate))
            continue;
        if (!other.activationFamily.isEmpty() && other.activationFamily != candidate.activationFamily)
            continue;
        if (other.lastUsed > latest)
            return false;
    }
    return true;
}

QCoro::Task<void> ProviderRegistry::startRestored(QStringList ids)
{
    // Home identities are decided newest-first, before another saved source
    // may reuse the in-memory authorization. Unrelated modules still start normally.
    QPointer<ProviderRegistry> guard(this);
    for (const auto& id : ids) {
        if (!guard)
            co_return;
        if (const auto *entry = account(id); entry && entry->enabled) {
            const auto *owner = module(entry->module);
            if (owner && owner->manifest.capabilities.contains(activationCapability))
                co_await start(id);
            else
                Async::runScoped(this, start(id), [] { }, [](const std::exception_ptr&) { }, "provider restore");
        }
    }
}

void ProviderRegistry::commitSelection(const ProviderAccount& candidate)
{
    m_awaitingChoice.remove(profileSet(candidate));
    QStringList retired;
    QStringList revoked;
    for (auto& other : m_accounts) {
        if (other.id == candidate.id || other.module != candidate.module)
            continue;
        const bool family
            = !candidate.activationFamily.isEmpty() && other.activationFamily == candidate.activationFamily;
        const bool conflict = family ? other.activationIdentity != candidate.activationIdentity
                                     : !candidate.group.isEmpty() && other.group == candidate.group;
        if (conflict) {
            other.enabled = false;
            retired.append(other.id);
            if (family && m_running.value(other.id).provider)
                revoked.append(other.id);
        }
    }
    for (const auto& id : revoked)
        emit accountIdentityRevoked(id);
    for (const auto& id : retired)
        stop(id);
}

QCoro::Task<void> ProviderRegistry::start(
    QString accountId, QString reason, bool select, std::optional<ProviderAccount> replacement)
{
    const auto *saved = account(accountId);
    bool selected = false;
    QPointer<ProviderRegistry> selectionGuard(this);
    const auto selectionFinished = qScopeGuard([selectionGuard, accountId, select, &selected] {
        if (selectionGuard && select)
            emit selectionGuard->accountSelectionFinished(accountId, selected);
    });
    if ((!saved && !replacement) || m_preparing.contains(accountId))
        co_return;
    ProviderAccount candidate = replacement ? *replacement : *saved;
    if (!m_modules.contains(candidate.module))
        co_return;
    const bool linked = reason == QStringLiteral("linked") && replacement.has_value();
    const bool hadAccount = saved != nullptr;
    if (saved) {
        candidate.activationFamily = saved->activationFamily;
        candidate.activationIdentity = saved->activationIdentity;
    }
    if (!select && !replacement && m_running.value(accountId).provider)
        co_return;
    ProviderModule& module = m_modules[candidate.module];
    if (m_unavailableConfigurations.contains(accountId)
        && (!replacement || !select || candidate.configuration.isEmpty())) {
        emit problem(QStringLiteral("Saved configuration for %1 is unavailable. Unlock your credential store "
                                    "and restart Spool, or sign in again.")
                .arg(candidate.label));
        co_return;
    }
    m_failedAccounts.remove(accountId);
    m_lockedAccounts.remove(accountId);
    m_accountErrors.remove(accountId);
    const QString preparedId = QStringLiteral("prepare-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const quint64 generation = ++m_nextGeneration;
    Running prepared;
    prepared.module = candidate.module;
    prepared.runtimeId = preparedId;
    prepared.accountId = accountId;
    prepared.generation = generation;
    prepared.origins = candidate.origins;
    prepared.enableOnCommit = select || candidate.enabled;
    for (const QString& origin : module.manifest.origins)
        prepared.origins.append(QUrl(origin));
    prepared.declaredCapabilities = ProviderCapabilityContract::declarations(module.manifest.capabilities);
    if (!candidate.activationFamily.isEmpty() || prepared.declaredCapabilities.value(activationCapability).toBool())
        prepared.activationApproval = std::make_shared<std::atomic_bool>(false);
    const auto native = module.native;
    QPointer<ScriptRuntime> runtime(native ? nullptr : runtimeFor(module));
    QPointer<ProviderRegistry> guard(this);
    m_preparing.insert(accountId, preparedId);
    m_running.insert(preparedId, prepared);
    m_runtimeSources.insert(preparedId, preparedId);
    const auto current = [&] {
        return guard && m_preparing.value(accountId) == preparedId
            && m_running.value(preparedId).generation == generation && (native || runtime);
    };
    const auto cleanup = qScopeGuard([guard, preparedId] {
        if (guard && guard->m_running.contains(preparedId))
            guard->stop(preparedId);
    });
    emit accountsChanged();
    QVariantMap description;
    QVariant grant;
    bool authorizedFamily = false;
    std::unique_ptr<Provider> nativeCandidate;
    try {
        if (!native) {
            const auto savedOptions = m_activationOptions.value(familyKey(candidate)).toMap();
            candidate.configuration.insert(savedOptions);
            co_await runtime->addSource(preparedId, candidate.configuration, prepared.origins,
                prepared.declaredCapabilities, false, prepared.activationApproval);
            if (!current())
                co_return;
            description = co_await runtime->call(preparedId, QStringLiteral("describe"));
            if (!current())
                co_return;
            readActivation(candidate, description);
            updateCapabilities(preparedId,
                description.contains(QStringLiteral("capabilities"))
                    ? ProviderCapabilityContract::decodeOffers(description.value(QStringLiteral("capabilities")))
                    : QVariantMap {});
            const bool hasFamily = !candidate.activationFamily.isEmpty();
            const bool requiresActivation
                = m_running.value(preparedId).capabilities.value(activationCapability).toBool();
            if (hasFamily && !requiresActivation)
                throw std::runtime_error("unsupported_capability");
            if (requiresActivation) {
                const quint64 epoch = hasFamily ? m_familyEpochs[familyKey(candidate)] : 0;
                // Metadata alone is not permission, but remembers the boundary even
                // when this source stays locked or a PIN is cancelled.
                if (auto *entry = account(accountId); hasFamily && entry && entry->activationFamily.isEmpty()) {
                    entry->activationFamily = candidate.activationFamily;
                    entry->activationIdentity = candidate.activationIdentity;
                    persist();
                }
                const auto options = m_activationOptions.value(familyKey(candidate)).toMap();
                if (options != savedOptions) {
                    candidate.configuration.insert(options);
                    m_running[preparedId].pendingConfiguration.clear();
                    m_running[preparedId].pendingEvents.clear();
                    m_running[preparedId].pendingOptions.clear();
                    runtime->removeSource(preparedId);
                    co_await runtime->addSource(preparedId, candidate.configuration, prepared.origins,
                        prepared.declaredCapabilities, false, prepared.activationApproval);
                    if (!current())
                        co_return;
                    description = co_await runtime->call(preparedId, QStringLiteral("describe"));
                    if (!current())
                        co_return;
                    readActivation(candidate, description);
                    updateCapabilities(preparedId,
                        description.contains(QStringLiteral("capabilities"))
                            ? ProviderCapabilityContract::decodeOffers(
                                  description.value(QStringLiteral("capabilities")))
                            : QVariantMap {});
                }
                if (!m_running.value(preparedId).capabilities.value(activationCapability).toBool())
                    throw std::runtime_error("unsupported_capability");
                const quint64 activationRevision
                    = m_running.value(preparedId).capabilityRevisions.value(activationCapability);
                const auto requireActivation = [&] {
                    if (!m_running.value(preparedId).capabilities.value(activationCapability).toBool()
                        || m_running.value(preparedId).capabilityRevisions.value(activationCapability)
                            != activationRevision)
                        throw std::runtime_error("unsupported_capability");
                };
                const QString activationScope = QUuid::createUuid().toString(QUuid::WithoutBraces);
                const quint64 activationCall = ++m_nextCapabilityCall;
                m_running[preparedId].capabilityCalls.insert(activationCall, { activationCapability, activationScope });
                const auto finishActivation = qScopeGuard([guard, preparedId, generation, activationCall] {
                    if (guard && guard->m_running.value(preparedId).generation == generation)
                        guard->m_running[preparedId].capabilityCalls.remove(activationCall);
                });
                const auto cached = m_activationGrants.value(familyKey(candidate));
                const bool family = hasFamily && cached.runtime == runtime && cached.generation != 0
                    && cached.identity == candidate.activationIdentity;
                const bool lastUsed = lastUsedIdentity(candidate);
                if (hasFamily && (reason == QStringLiteral("startup") || reason == QStringLiteral("restart"))
                    && !lastUsed && !family && !startupDefault(candidate)) {
                    m_lockedAccounts.insert(accountId);
                    co_return;
                }
                QVariantMap arguments { { QStringLiteral("reason"),
                                            linked                                    ? QStringLiteral("linked")
                                                : family                              ? QStringLiteral("family")
                                                : reason == QStringLiteral("restart") ? QStringLiteral("startup")
                                                                                      : reason },
                    { QStringLiteral("lastUsed"), lastUsed } };
                if (family && cached.value.isValid())
                    arguments.insert(QStringLiteral("grant"), cached.value);
                bool activated = false;
                for (int attempt = 0; attempt < 8; ++attempt) {
                    const auto result
                        = co_await runtime->call(preparedId, QStringLiteral("activate"), arguments, activationScope);
                    if (!current())
                        co_return;
                    requireActivation();
                    if (result.contains(QStringLiteral("pick"))) {
                        const QVariant picker = result.value(QStringLiteral("pick"));
                        if (picker.metaType().id() != QMetaType::QVariantMap)
                            throw std::runtime_error("invalid_activation");
                        // Background restore never interrupts Home with a profile/PIN
                        // chooser. Protected viewers stay locked until explicitly chosen.
                        if (reason == QStringLiteral("startup")) {
                            m_lockedAccounts.insert(accountId);
                            co_return;
                        }
                        const auto answer = co_await pickResult(preparedId, picker.toMap(), true, activationScope);
                        if (!current())
                            co_return;
                        requireActivation();
                        if (!answer.submitted) {
                            m_lockedAccounts.insert(accountId);
                            co_return;
                        }
                        // No picker value can replace reason, identity, or proof.
                        arguments.insert(QStringLiteral("answers"), answer.values);
                        continue;
                    }
                    if (result.contains(QStringLiteral("grant"))) {
                        grant = result.value(QStringLiteral("grant"));
                        ProviderExtensionData::validateValue(grant, 16 * 1024, "invalid_activation");
                    }
                    activated = true;
                    break;
                }
                if (!activated)
                    throw std::runtime_error("invalid_activation");
                description = co_await runtime->call(preparedId, QStringLiteral("describe"));
                if (!current())
                    co_return;
                requireActivation();
                readActivation(candidate, description);
                if (hasFamily != !candidate.activationFamily.isEmpty()
                    || (hasFamily && m_familyEpochs.value(familyKey(candidate)) != epoch))
                    throw std::runtime_error("source_changed");
                updateCapabilities(preparedId,
                    description.contains(QStringLiteral("capabilities"))
                        ? ProviderCapabilityContract::decodeOffers(description.value(QStringLiteral("capabilities")))
                        : QVariantMap {});
                requireActivation();
                authorizedFamily = hasFamily;
            }
        } else {
            nativeCandidate.reset(native(accountId, candidate.configuration, this));
        }
    } catch (const std::exception& error) {
        if (!current())
            co_return;
        const bool signIn = requiresSignIn(error);
        if (signIn)
            m_expired.insert(accountId);
        const QString message = activationError(error);
        m_accountErrors.insert(accountId, message);
        // A rejected PIN leaves the profile waiting for its unlock; anything
        // else is a failure to open it.
        if (!m_running.value(accountId).provider) {
            if (std::string_view(error.what()) == "invalid_pin")
                m_lockedAccounts.insert(accountId);
            else
                m_failedAccounts.insert(accountId);
        }
        // Provider errors may contain credentials; do not log candidate failures.
        // Explicit setup and switches report beside their actionable profile tile.
        if (!select)
            emit problem(QStringLiteral("%1: %2").arg(candidate.label, message));
        co_return;
    }
    if (!current())
        co_return;
    candidate.configuration.insert(m_running.value(preparedId).pendingConfiguration);
    if (select) {
        candidate.enabled = true;
        candidate.lastUsed = QDateTime::currentMSecsSinceEpoch();
        for (const auto& other : m_accounts)
            candidate.lastUsed = std::max(candidate.lastUsed, other.lastUsed + 1);
    }
    if (select || authorizedFamily)
        commitSelection(candidate);
    stopPublished(accountId);
    if (!current())
        co_return;
    if (auto *entry = account(accountId))
        *entry = candidate;
    else
        m_accounts.push_back(candidate);
    Running active = m_running.take(preparedId);
    m_preparing.remove(accountId);
    m_runtimeSources[preparedId] = accountId;
    const auto pendingEvents = std::move(active.pendingEvents);
    const auto pendingOptions = std::move(active.pendingOptions);
    active.pendingConfiguration.clear();
    if (active.activationApproval)
        active.activationApproval->store(true);
    active.provider = native ? nativeCandidate.release()
                             : new PortableProvider(this, accountId, candidate.label,
                                   capabilitiesOf(active.capabilities), description, this);
    m_running.insert(accountId, std::move(active));
    if (authorizedFamily) {
        clearGrants(candidate.module, candidate.activationFamily);
        m_activationGrants.insert(familyKey(candidate), { candidate.activationIdentity, runtime, generation, grant });
    }
    m_lockedAccounts.remove(accountId);
    m_unavailableConfigurations.remove(accountId);
    m_expired.remove(accountId);
    m_accountErrors.remove(accountId);
    persist(true);
    emit sourceStarted(m_running[accountId].provider);
    emit capabilitiesChanged(accountId);
    // Choosing a viewer brings their other servers in this family along,
    // authorized by the proof just granted rather than another prompt.
    QStringList siblings;
    if (select && authorizedFamily) {
        for (ProviderAccount& other : m_accounts) {
            if (other.id != accountId && sameProfile(other, candidate) && !m_running.value(other.id).provider
                && !m_preparing.contains(other.id)) {
                other.enabled = true;
                siblings.append(other.id);
            }
        }
        if (!siblings.isEmpty())
            persist();
    }
    emit accountsChanged();
    for (const QString& sibling : std::as_const(siblings))
        Async::runScoped(this, start(sibling), [] { }, [](const std::exception_ptr&) { }, "provider family");
    selected = select;
    if (linked || !hadAccount || (select && replacement))
        emit accountAdded(accountId);
    if (!pendingOptions.isEmpty())
        handleEvent(preparedId, QStringLiteral("activationConfiguration"),
            { { QStringLiteral("configuration"), pendingOptions } });
    for (const auto& event : pendingEvents)
        handleEvent(preparedId, event.first, event.second);
}

void ProviderRegistry::stopPublished(const QString& sourceId)
{
    const auto running = m_running.find(sourceId);
    if (running == m_running.end())
        return;
    const Running state = running.value();
    m_running.erase(running);
    if (state.activationApproval)
        state.activationApproval->store(false);
    m_runtimeSources.remove(state.runtimeId);
    if (m_preparing.value(state.accountId) == sourceId)
        m_preparing.remove(state.accountId);
    cancelNetworkConsent(sourceId);
    if (ScriptRuntime *runtime = m_modules.value(state.module).runtime)
        runtime->removeSource(state.runtimeId);
    emit contextSourceStopped(sourceId);
    emit capabilitiesChanged(sourceId);
    if (state.provider) {
        emit sourceStopped(sourceId);
        state.provider->shutdown();
        state.provider->deleteLater();
    }
    emit accountsChanged();
}

void ProviderRegistry::stop(const QString& sourceId, bool revokeIdentity)
{
    const QString prepared = m_preparing.value(sourceId);
    if (!prepared.isEmpty())
        stopPublished(prepared);
    if (revokeIdentity && m_running.value(sourceId).provider) {
        const auto *entry = account(sourceId);
        if (entry && !entry->activationFamily.isEmpty())
            emit accountIdentityRevoked(sourceId);
    }
    stopPublished(sourceId);
}

void ProviderRegistry::restartModule(const QString& moduleId)
{
    clearGrants(moduleId);
    for (const QString& id : m_running.keys()) {
        if (m_running.value(id).module == moduleId)
            stop(id, true);
    }
    ProviderModule& module = m_modules[moduleId];
    delete std::exchange(module.runtime, nullptr);
    module.failed = false;
    QStringList ids;
    for (const ProviderAccount& account : m_accounts) {
        if (account.enabled && account.module == moduleId)
            ids.append(account.id);
    }
    std::stable_sort(ids.begin(), ids.end(),
        [this](const QString& a, const QString& b) { return account(a)->lastUsed > account(b)->lastUsed; });
    Async::runScoped(this, startRestored(ids), [] { }, [](const std::exception_ptr&) { }, "provider restart");
}

void ProviderRegistry::persist(bool credentials)
{
    QJsonArray rows;
    QHash<QString, QString> secrets;
    for (const ProviderAccount& account : m_accounts) {
        rows.append(toJson(account));
        // Never replace a temporarily unreadable keyring entry with {} when
        // another account starts, changes settings, or is removed.
        if (m_unavailableConfigurations.contains(account.id))
            continue;
        secrets.insert(account.id,
            QString::fromUtf8(
                QJsonDocument(QJsonObject::fromVariantMap(account.configuration)).toJson(QJsonDocument::Compact)));
    }
    if (credentials) {
        QStringList removed = std::exchange(m_removedAccounts, {});
        // One thread, so saves land in the order they were made.
        Async::background(
            [secrets, removed] {
                for (auto it = secrets.cbegin(); it != secrets.cend(); ++it)
                    CredentialStore::save(it.key(), it.value());
                for (const QString& id : removed)
                    CredentialStore::remove(id);
            },
            &m_credentialPool);
    }
    m_database->saveSetting(kAccountsKey,
        QString::fromUtf8(
            QJsonDocument(QJsonObject { { QStringLiteral("format"), 2 }, { QStringLiteral("accounts"), rows },
                              { QStringLiteral("activationOptions"), QJsonObject::fromVariantMap(m_activationOptions) },
                              { QStringLiteral("startup"), QJsonObject::fromVariantMap(m_startupChoices) } })
                .toJson(QJsonDocument::Compact)));
}

QCoro::Task<void> ProviderRegistry::install(ProviderPackageContents package)
{
    const QString root = m_installDirectory;
    if (root.isEmpty())
        throw std::runtime_error("installs_disabled");
    // Disk writes stay off the GUI thread.
    QString error;
    const auto directory = co_await Async::background(
        [package, root, &error] { return ProviderPackage::install(package, root, &error); });
    if (!directory)
        throw std::runtime_error(error.toStdString());
    const QString id = package.manifest.id;
    const ScriptRuntime *before = m_modules.contains(id) ? m_modules[id].runtime : nullptr;
    registerPackage(package.manifest, QUrl::fromLocalFile(*directory + QLatin1Char('/')), false);
    if (before || m_modules[id].runtime)
        restartModule(id);
    emit modulesChanged();
    emit accountsChanged();
}

QCoro::Task<void> ProviderRegistry::uninstall(QString moduleId)
{
    clearGrants(moduleId);
    const ProviderModule *existing = module(moduleId);
    if (!existing || (existing->bundled && !existing->overridesBundled) || existing->native)
        co_return;
    for (const QString& id : m_running.keys()) {
        if (m_running.value(id).module == moduleId)
            stop(id, true);
    }
    delete std::exchange(m_modules[moduleId].runtime, nullptr);
    const bool hadBundled = existing->overridesBundled;
    m_modules.remove(moduleId);
    const QString directory = QDir(m_installDirectory).filePath(moduleId);
    co_await Async::background([directory] { return QDir(directory).removeRecursively(); });
    if (hadBundled) {
        loadModules();
        restartModule(moduleId);
    } else {
        for (const ProviderAccount& account : m_accounts) {
            if (account.module == moduleId)
                m_removedAccounts.append(account.id);
        }
        std::erase_if(m_accounts, [&](const ProviderAccount& account) { return account.module == moduleId; });
        persist(true);
    }
    emit modulesChanged();
    emit accountsChanged();
}

template <typename T, typename Call>
QCoro::Task<T> ProviderRegistry::guarded(QString sourceId, Call call, QString capability, QString scope)
{
    const auto running = m_running.constFind(sourceId);
    ScriptRuntime *runtime = running == m_running.cend() ? nullptr : m_modules.value(running->module).runtime;
    if (!runtime)
        throw std::runtime_error("source_unavailable");
    if (!running->draft && !running->provider)
        throw std::runtime_error("source_unavailable");
    if (!capability.isEmpty() && !sourceHasCapability(sourceId, capability))
        throw std::runtime_error("unsupported_capability");
    const quint64 generation = running->generation;
    const QString runtimeId = running->runtimeId;
    QPointer<ProviderRegistry> guard(this);
    const quint64 revision = running->capabilityRevisions.value(capability);
    const quint64 callId = capability.isEmpty() ? 0 : ++m_nextCapabilityCall;
    if (callId)
        m_running[sourceId].capabilityCalls.insert(callId, { capability, scope });
    const auto cleanup = qScopeGuard([guard, sourceId, generation, callId] {
        if (guard) {
            auto current = guard->m_running.find(sourceId);
            if (current != guard->m_running.end() && current->generation == generation)
                current->capabilityCalls.remove(callId);
        }
    });
    std::optional<T> result;
    try {
        result = co_await call(runtime, runtimeId);
    } catch (const std::exception& error) {
        // The server no longer accepts this account's credentials: say so
        // once, and let the accounts page offer to sign in again.
        if (guard && m_running.value(sourceId).generation == generation && requiresSignIn(error)
            && !m_expired.contains(sourceId)) {
            m_expired.insert(sourceId);
            if (const ProviderAccount *entry = account(sourceId))
                emit problem(QStringLiteral("Sign in to %1 again").arg(entry->label));
            emit accountsChanged();
        }
        throw;
    }
    if (!guard || m_running.value(sourceId).generation != generation)
        throw std::runtime_error("source_changed");
    if (!capability.isEmpty()
        && (m_running.value(sourceId).capabilityRevisions.value(capability) != revision
            || !sourceHasCapability(sourceId, capability)))
        throw std::runtime_error("unsupported_capability");
    co_return std::move(*result);
}

QCoro::Task<QVariantMap> ProviderRegistry::callSource(
    QString sourceId, QString operation, QVariantMap arguments, QString scope)
{
    if (operation == QStringLiteral("activate"))
        throw std::runtime_error("action_unavailable");
    const QString capability = ProviderCapabilityContract::operationCapability(operation);
    if (!capability.isEmpty() && !sourceHasCapability(sourceId, capability))
        throw std::runtime_error("unsupported_capability");
    if (!capability.isEmpty() && scope.isEmpty())
        scope = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const bool preferences = capability == QStringLiteral("playbackPreferences");
    const bool storage = capability == QStringLiteral("settingsStorage");
    const QPointer<ProviderRegistry> owner(this);
    const quint64 generation = m_running.value(sourceId).generation;
    const quint64 revision = m_running.value(sourceId).capabilityRevisions.value(capability);
    const auto requireCurrent = [owner, sourceId, capability, generation, revision] {
        if (!owner || owner->m_running.value(sourceId).generation != generation)
            throw std::runtime_error("source_changed");
        if (!capability.isEmpty()
            && (!owner->sourceHasCapability(sourceId, capability)
                || owner->m_running.value(sourceId).capabilityRevisions.value(capability) != revision))
            throw std::runtime_error("unsupported_capability");
    };
    ProviderExtensionData::StorageInfo info;
    if (preferences) {
        arguments = ProviderExtensionData::preferenceArguments(operation, arguments);
        if (operation == QStringLiteral("preferencesWrite")) {
            const auto current = co_await callSource(sourceId, QStringLiteral("preferencesRead"), {}, scope);
            requireCurrent();
            ProviderExtensionData::requireWritable(arguments.value(QStringLiteral("values")).toMap(), current);
        }
    } else if (storage) {
        // Validate locally before even discovering the backend limits.
        ProviderExtensionData::storageArguments(operation, arguments);
        if (operation != QStringLiteral("dataInfo")) {
            if (!m_running.value(sourceId).storageInfo) {
                co_await callSource(sourceId, QStringLiteral("dataInfo"), {}, scope);
                requireCurrent();
            }
            info = *m_running.value(sourceId).storageInfo;
            ProviderExtensionData::storageArguments(operation, arguments, &info);
        } else if (m_running.value(sourceId).storageInfo) {
            info = *m_running.value(sourceId).storageInfo;
            co_return QVariantMap { { QStringLiteral("maxBytes"), info.maxBytes },
                { QStringLiteral("conditionalWrites"), info.conditionalWrites } };
        }
    }
    auto result = co_await guarded<QVariantMap>(
        sourceId,
        [=](ScriptRuntime *runtime, const QString& runtimeId) {
            return runtime->call(runtimeId, operation, arguments, scope);
        },
        capability, scope);
    requireCurrent();
    if (preferences)
        result = ProviderExtensionData::preferenceResult(operation, result);
    else if (storage) {
        result = ProviderExtensionData::storageResult(operation, result, info);
        if (operation == QStringLiteral("dataInfo"))
            m_running[sourceId].storageInfo = ProviderExtensionData::storageInfo(result);
    }
    co_return result;
}

QCoro::Task<ProviderMediaPage> ProviderRegistry::callSourceMediaPage(
    QString sourceId, QString operation, QVariantMap arguments, int maximumItems, QString scope)
{
    if (operation == QStringLiteral("activate"))
        throw std::runtime_error("action_unavailable");
    const QString capability = ProviderCapabilityContract::operationCapability(operation);
    // Preference and storage data must pass their bounded object validators.
    if (capability == QStringLiteral("playbackPreferences") || capability == QStringLiteral("settingsStorage"))
        throw std::runtime_error("unsupported_capability");
    if (!capability.isEmpty() && scope.isEmpty())
        scope = QUuid::createUuid().toString(QUuid::WithoutBraces);
    co_return co_await guarded<ProviderMediaPage>(
        sourceId,
        [=](ScriptRuntime *runtime, const QString& runtimeId) {
            return runtime->callMediaPage(runtimeId, operation, arguments, scope, maximumItems);
        },
        capability, scope);
}

QCoro::Task<MovieItem> ProviderRegistry::callSourceItem(QString sourceId, QString operation, QVariantMap arguments)
{
    if (operation == QStringLiteral("activate"))
        throw std::runtime_error("action_unavailable");
    const QString capability = ProviderCapabilityContract::operationCapability(operation);
    if (capability == QStringLiteral("playbackPreferences") || capability == QStringLiteral("settingsStorage"))
        throw std::runtime_error("unsupported_capability");
    const QString scope = capability.isEmpty() ? QString() : QUuid::createUuid().toString(QUuid::WithoutBraces);
    co_return co_await guarded<MovieItem>(
        sourceId,
        [=](ScriptRuntime *runtime, const QString& runtimeId) {
            return runtime->callItem(runtimeId, operation, arguments, scope);
        },
        capability, scope);
}

bool ProviderRegistry::hasCapability(const QString& accountId, const QString& capability) const
{
    const auto running = m_running.constFind(accountId);
    return running != m_running.cend() && running->provider && !running->draft
        && running->capabilities.value(capability).toBool();
}

bool ProviderRegistry::sourceHasCapability(const QString& sourceId, const QString& capability) const
{
    const auto running = m_running.constFind(sourceId);
    if (running == m_running.cend())
        return false;
    // Login discovery uses declarations. Only the HTTP LAN probe continuation
    // requires its existing separate consent; UDP discovery keeps its policy.
    return running->draft ? (capability == QStringLiteral("discovery") || capability == QStringLiteral("lanProbe"))
            && running->declaredCapabilities.value(capability).toBool()
                          : hasCapability(sourceId, capability);
}

QVariantMap ProviderRegistry::capabilities(const QString& sourceId) const
{
    const auto running = m_running.constFind(sourceId);
    if (running == m_running.cend())
        return {};
    return running->draft                                                        ? running->declaredCapabilities
        : running->provider || m_preparing.value(running->accountId) == sourceId ? running->capabilities
                                                                                 : QVariantMap {};
}

void ProviderRegistry::updateCapabilities(const QString& sourceId, QVariantMap offers)
{
    auto running = m_running.find(sourceId);
    if (running == m_running.end() || running->draft)
        return;
    const QVariantMap effective = ProviderCapabilityContract::intersect(running->declaredCapabilities, offers);
    if (effective == running->capabilities)
        return;
    QStringList lost;
    for (auto it = running->capabilities.cbegin(); it != running->capabilities.cend(); ++it) {
        if (!effective.value(it.key()).toBool()) {
            lost.append(it.key());
            ++running->capabilityRevisions[it.key()];
        }
    }
    running->capabilities = effective;
    if (lost.contains(QStringLiteral("settingsStorage")))
        running->storageInfo.reset();
    QSet<QString> scopes;
    for (const CapabilityCall& call : running->capabilityCalls) {
        if (lost.contains(call.capability))
            scopes.insert(call.scope);
    }
    const QPointer<PortableProvider> provider = qobject_cast<PortableProvider *>(running->provider.data());
    const QPointer<ScriptRuntime> runtime = m_modules.value(running->module).runtime;
    const QString runtimeId = running->runtimeId;
    if (lost.contains(QStringLiteral("originGrants")))
        cancelNetworkConsent(sourceId);
    if (runtime) {
        for (const QString& scope : scopes)
            runtime->cancelScope(runtimeId, scope);
    }
    if (provider)
        provider->setCapabilities(capabilitiesOf(effective));
    for (const QString& scope : scopes)
        emit sourceScopeCancelled(sourceId, scope);
    emit capabilitiesChanged(sourceId);
    emit accountsChanged();
}

void ProviderRegistry::cancelSourceScope(const QString& sourceId, const QString& scope)
{
    emit sourceScopeCancelled(sourceId, scope);
    cancelNetworkConsent(sourceId, scope);
    auto state = m_running.find(sourceId);
    if (state != m_running.end())
        ++state->networkRevision;
    const auto running = m_running.constFind(sourceId);
    if (running != m_running.cend()) {
        if (ScriptRuntime *runtime = m_modules.value(running->module).runtime)
            runtime->cancelScope(running->runtimeId, scope);
    }
}

bool ProviderRegistry::sourceRunning(const QString& sourceId) const
{
    const auto running = m_running.constFind(sourceId);
    return running != m_running.cend() && (running->draft || running->provider);
}

bool ProviderRegistry::accountOriginAllowed(const QString& accountId, const QUrl& url) const
{
    const auto running = m_running.constFind(accountId);
    if (running == m_running.cend() || running->draft || !running->provider)
        return false;
    QUrl candidate;
    try {
        candidate = grantOrigin(url);
    } catch (const std::exception&) {
        return false;
    }
    for (const QUrl& approved : running->origins) {
        if (approved.toString() == QStringLiteral("*"))
            continue;
        try {
            if (candidate == grantOrigin(approved))
                return true;
        } catch (const std::exception&) {
            // Wildcard/malformed legacy grants never authorize a remote preview.
        }
    }
    return false;
}

void ProviderRegistry::handleEvent(const QString& runtimeId, const QString& type, const QVariantMap& payload)
{
    const QString sourceId = m_runtimeSources.value(runtimeId);
    auto state = m_running.find(sourceId);
    if (state == m_running.end())
        return;
    if (type == QStringLiteral("capabilitiesChanged")) {
        try {
            updateCapabilities(
                sourceId, ProviderCapabilityContract::decodeOffers(payload.value(QStringLiteral("capabilities"))));
        } catch (const std::exception&) {
            updateCapabilities(sourceId, {});
        }
        return;
    }
    if (!state->provider && !state->draft) {
        if (type == QStringLiteral("configuration")) {
            state->pendingConfiguration.insert(payload);
        } else if (type == QStringLiteral("activationConfiguration")) {
            try {
                state->pendingOptions.insert(activationOptions(payload.value(QStringLiteral("configuration"))));
            } catch (const std::exception&) {
            }
        } else if (state->pendingEvents.size() < 64) {
            state->pendingEvents.append({ type, payload });
        }
        return;
    }
    if (type == QStringLiteral("configuration")) {
        if (state->draft)
            state->pendingConfiguration.insert(payload);
        else
            updateConfiguration(sourceId, payload);
        return;
    }
    if (type == QStringLiteral("activationConfiguration")) {
        const auto *entry = account(sourceId);
        if (!entry || entry->activationFamily.isEmpty() || !hasCapability(sourceId, activationCapability))
            return;
        QVariantMap options;
        try {
            options = activationOptions(payload.value(QStringLiteral("configuration")));
        } catch (const std::exception&) {
            return;
        }
        const QString key = familyKey(*entry);
        auto combined = m_activationOptions.value(key).toMap();
        combined.insert(options);
        m_activationOptions.insert(key, combined);
        // Boolean-only, device-local provider options. Never grants or credentials.
        QStringList changedAccounts;
        for (auto& other : m_accounts) {
            if (familyKey(other) == key) {
                other.configuration.insert(options);
                changedAccounts.append(other.id);
            }
        }
        persist(true);
        for (const QString& id : std::as_const(changedAccounts))
            emit activationConfigurationChanged(id);
        return;
    }
    const QPointer<Provider> provider = state->provider;
    if (!provider)
        return;
    if (type == QStringLiteral("changed"))
        emit provider->contentChanged(payload.value(QStringLiteral("itemId")).toString());
    else
        emit provider->sourceEvent(type, payload);
}

void ProviderRegistry::handleInterrupted(const QString& moduleId)
{
    clearGrants(moduleId);
    ProviderModule& module = m_modules[moduleId];
    module.failed = true;
    for (const QString& id : m_running.keys()) {
        if (m_running.value(id).module == moduleId) {
            const auto state = m_running.value(id);
            if (!state.accountId.isEmpty())
                m_failedAccounts.insert(state.accountId);
            stop(id, true);
        }
    }
    emit problem(QStringLiteral("%1 stopped responding and was turned off").arg(module.manifest.name));
    emit modulesChanged();
    emit accountsChanged();
}

QVariantList ProviderRegistry::modules() const
{
    QVariantList list;
    for (const ProviderModule& module : m_modules) {
        const auto count = std::count_if(m_accounts.begin(), m_accounts.end(),
            [&](const auto& account) { return account.module == module.manifest.id; });
        list.append(QVariantMap { { QStringLiteral("id"), module.manifest.id },
            { QStringLiteral("name"), module.manifest.name }, { QStringLiteral("summary"), module.manifest.summary },
            { QStringLiteral("version"), module.manifest.version },
            { QStringLiteral("publisher"), module.manifest.publisher },
            { QStringLiteral("homepage"), module.manifest.homepage },
            { QStringLiteral("iconUrl"), module.file(module.manifest.icon) },
            { QStringLiteral("bundled"), module.bundled && !module.overridesBundled },
            { QStringLiteral("removable"), !module.native && (!module.bundled || module.overridesBundled) },
            { QStringLiteral("needsAccount"), module.manifest.needsAccount() },
            { QStringLiteral("accountCount"), int(count) }, { QStringLiteral("failed"), module.failed } });
    }
    std::sort(list.begin(), list.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap()
                   .value(QStringLiteral("name"))
                   .toString()
                   .localeAwareCompare(b.toMap().value(QStringLiteral("name")).toString())
            < 0;
    });
    return list;
}

QVariantList ProviderRegistry::accounts() const
{
    QVariantList list;
    for (const ProviderAccount& account : m_accounts) {
        const ProviderModule *owner = module(account.module);
        // Where the account's server is, for telling servers apart: the host
        // and port of its configured address, never the rest of the URL.
        const QUrl server(account.configuration.value(QStringLiteral("server")).toString(), QUrl::StrictMode);
        const QString address = server.isValid()
                && (server.scheme() == QLatin1String("http") || server.scheme() == QLatin1String("https"))
            ? server.host() + (server.port() > 0 ? QLatin1Char(':') + QString::number(server.port()) : QString())
            : QString();
        const QString set = profileSet(account);
        const auto startup = m_startupChoices.value(set).toMap();
        const bool awaiting = m_awaitingChoice.contains(set);
        list.append(QVariantMap { { QStringLiteral("id"), account.id }, { QStringLiteral("moduleId"), account.module },
            { QStringLiteral("address"), address }, { QStringLiteral("profileSet"), set },
            // One tile per person: an activation identity spans its servers.
            { QStringLiteral("profile"),
                std::find_if(m_accounts.cbegin(), m_accounts.cend(),
                    [&](const ProviderAccount& entry) { return sameProfile(entry, account); })
                    ->id },
            { QStringLiteral("profiles"), owner && owner->manifest.needsAccount() },
            { QStringLiteral("startupMode"), startup.value(QStringLiteral("mode")).toString() },
            { QStringLiteral("startupDefault"), startupDefault(account) },
            { QStringLiteral("onboarding"), m_onboarding.contains(account.id) },
            { QStringLiteral("pending"), m_preparing.contains(account.id) },
            { QStringLiteral("removing"), m_removingAccounts.contains(account.id) },
            // Waiting for its PIN or other activation, not merely set aside.
            { QStringLiteral("locked"), m_lockedAccounts.contains(account.id) },
            { QStringLiteral("providerName"), owner ? owner->manifest.name : account.module },
            { QStringLiteral("iconUrl"), owner ? owner->file(owner->manifest.icon) : QUrl() },
            { QStringLiteral("label"), account.label }, { QStringLiteral("detail"), account.detail },
            { QStringLiteral("group"), account.group }, { QStringLiteral("enabled"), account.enabled },
            { QStringLiteral("running"), bool(m_running.value(account.id).provider) },
            { QStringLiteral("pendingEnabled"),
                m_preparing.contains(account.id) && m_running.value(m_preparing.value(account.id)).enableOnCommit },
            { QStringLiteral("connectionState"),
                m_running.value(account.id).provider                                    ? QStringLiteral("active")
                    : m_preparing.contains(account.id)                                  ? QStringLiteral("starting")
                    : m_lockedAccounts.contains(account.id)                             ? QStringLiteral("locked")
                    : m_failedAccounts.contains(account.id) || (owner && owner->failed) ? QStringLiteral("failed")
                    : awaiting                                                          ? QStringLiteral("choose")
                    : account.enabled                                                   ? QStringLiteral("starting")
                                                                                        : QStringLiteral("locked") },
            { QStringLiteral("capabilities"), capabilities(account.id) },
            { QStringLiteral("needsSignIn"),
                m_expired.contains(account.id) || m_unavailableConfigurations.contains(account.id) },
            { QStringLiteral("errorText"), m_accountErrors.value(account.id) },
            { QStringLiteral("hasSettings"), owner && owner->manifest.ui.contains(QStringLiteral("settings")) } });
    }
    return list;
}

QUrl ProviderRegistry::componentUrl(const QString& moduleId, const QString& role) const
{
    const ProviderModule *owner = module(moduleId);
    return owner ? owner->file(owner->manifest.ui.value(role)) : QUrl();
}

ProviderUiContext *ProviderRegistry::createContext(
    const QString& sourceId, const QString& role, const QString& moduleId)
{
    const QUrl component = componentUrl(moduleId, role);
    if (component.isEmpty())
        return nullptr;
    auto *context = new ProviderUiContext(this, sourceId, moduleId, role, component);
    connect(this, &ProviderRegistry::contextSourceStopped, context, [context](const QString& id) {
        if (id == context->sourceId())
            context->close();
    });
    return context;
}

QObject *ProviderRegistry::beginSetup(const QString& moduleId, const QString& accountId, const QString& purpose)
{
    ProviderModule *owner = m_modules.contains(moduleId) ? &m_modules[moduleId] : nullptr;
    if (!owner)
        return nullptr;
    const ProviderAccount *existingAccount = accountId.isEmpty() ? nullptr : account(accountId);
    if (!accountId.isEmpty() && (!existingAccount || existingAccount->module != moduleId))
        return nullptr;
    QVariantMap setupContext;
    QVariantMap setupConfiguration;
    if (existingAccount) {
        setupContext = { { QStringLiteral("accountId"), accountId }, { QStringLiteral("purpose"), purpose },
            { QStringLiteral("serverId"), existingAccount->group },
            { QStringLiteral("serverName"), existingAccount->detail },
            { QStringLiteral("serverOrigin"),
                existingAccount->origins.isEmpty() ? QString() : existingAccount->origins.front().toString() } };
        setupConfiguration = { { QStringLiteral("setupContext"), setupContext },
            { QStringLiteral("setupAccount"), existingAccount->configuration } };
    }
    if (!owner->manifest.needsAccount()) {
        const auto existing = std::find_if(
            m_accounts.begin(), m_accounts.end(), [&](const auto& account) { return account.module == moduleId; });
        const bool alreadyAdded = existing != m_accounts.end();
        const QString id = alreadyAdded
            ? existing->id
            : finishSetup({},
                  { { QStringLiteral("module"), moduleId }, { QStringLiteral("account"), QStringLiteral("default") },
                      { QStringLiteral("label"), owner->manifest.name } });
        if (alreadyAdded)
            useAccount(id);
        return nullptr;
    }
    const QString draftId = QStringLiteral("setup-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QList<QUrl> origins;
    for (const QString& origin : owner->manifest.origins)
        origins.append(QUrl(origin));
    if (existingAccount) {
        for (const QUrl& origin : existingAccount->origins) {
            if (!origins.contains(origin))
                origins.append(origin);
        }
    }
    Running draft;
    draft.module = moduleId;
    draft.runtimeId = draftId;
    draft.generation = ++m_nextGeneration;
    draft.origins = origins;
    draft.draft = true;
    draft.setupConfiguration = setupConfiguration;
    m_running.insert(draftId, draft);
    m_runtimeSources.insert(draftId, draftId);
    m_running[draftId].declaredCapabilities = ProviderCapabilityContract::declarations(owner->manifest.capabilities);
    if (!owner->native) {
        Async::runScoped(
            this,
            runtimeFor(*owner)->addSource(
                draftId, setupConfiguration, origins, m_running[draftId].declaredCapabilities, true),
            [](QVariantMap) {},
            [this, draftId](const std::exception_ptr&) {
                stop(draftId);
                emit problem(QStringLiteral("This provider could not start"));
            },
            "provider setup");
    }
    auto *context = createContext(draftId, QStringLiteral("login"), moduleId);
    if (context)
        context->setArguments({ { QStringLiteral("setupContext"), setupContext } });
    return context;
}

QCoro::Task<void> ProviderRegistry::allowSetupOrigin(QString draftId, QUrl url)
{
    const auto running = m_running.find(draftId);
    if (running == m_running.end() || !running->draft)
        throw std::runtime_error("origin_denied");
    const QUrl origin = grantOrigin(url);
    if (running->origins.contains(origin))
        co_return;
    cancelNetworkConsent(draftId);
    const quint64 generation = running->generation;
    const quint64 revision = running->networkRevision;
    ScriptRuntime *runtime = m_modules.value(running->module).runtime;
    if (!runtime)
        throw std::runtime_error("source_unavailable");
    // Keep provider-private link/member/session state in the same draft closure.
    // The grant remains unusable until this context survives the worker round trip.
    auto approval = std::make_shared<std::atomic_bool>(false);
    QPointer<ProviderRegistry> guard(this);
    co_await runtime->grantOrigins(draftId, { origin }, approval);
    if (!guard || m_running.value(draftId).generation != generation
        || m_running.value(draftId).networkRevision != revision)
        throw std::runtime_error("source_changed");
    approval->store(true);
    if (!m_running[draftId].origins.contains(origin))
        m_running[draftId].origins.append(origin);
}

QCoro::Task<bool> ProviderRegistry::requestNetworkConsent(QString sourceId, QString scope, QString kind, QUrl origin)
{
    // One host prompt at a time. A competing request never replaces what the
    // viewer is considering, and absence of a shell cannot imply approval.
    if (m_consentPromise)
        co_return false;
    const auto running = m_running.constFind(sourceId);
    if (running == m_running.cend())
        co_return false;
    const ProviderModule *owner = module(running->module);
    const ProviderAccount *entry = account(sourceId);
    auto promise = std::make_shared<QPromise<bool>>();
    promise->start();
    auto future = promise->future();
    m_consentPromise = promise;
    m_consentSource = sourceId;
    m_consentScope = scope;
    m_networkConsent = { { QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces) },
        { QStringLiteral("kind"), kind },
        { QStringLiteral("provider"), owner ? owner->manifest.name : running->module },
        { QStringLiteral("account"), entry ? entry->label : QString() },
        { QStringLiteral("origin"), origin.toString(QUrl::FullyEncoded) },
        { QStringLiteral("unencrypted"), origin.scheme() == QStringLiteral("http") } };
    emit networkConsentChanged();
    co_return co_await qCoro(future).result();
}

void ProviderRegistry::resolveNetworkConsent(const QString& requestId, bool approved)
{
    if (!m_consentPromise || m_networkConsent.value(QStringLiteral("id")).toString() != requestId)
        return;
    const auto promise = std::exchange(m_consentPromise, {});
    m_networkConsent.clear();
    m_consentSource.clear();
    m_consentScope.clear();
    emit networkConsentChanged();
    promise->addResult(approved);
    promise->finish();
}

void ProviderRegistry::cancelNetworkConsent(const QString& sourceId, const QString& scope)
{
    if (m_consentPromise && m_consentSource == sourceId && (scope.isEmpty() || m_consentScope == scope))
        resolveNetworkConsent(m_networkConsent.value(QStringLiteral("id")).toString(), false);
}

QCoro::Task<void> ProviderRegistry::requestAccountOrigin(QString accountId, QUrl url, QString scope)
{
    const QUrl origin = grantOrigin(url);
    const auto running = m_running.constFind(accountId);
    if (running == m_running.cend() || running->draft || !running->provider
        || !hasCapability(accountId, QStringLiteral("originGrants")))
        throw std::runtime_error("unsupported_capability");
    if (running->origins.contains(origin))
        co_return;
    const quint64 generation = running->generation;
    const quint64 revision = running->networkRevision;
    const quint64 capabilityRevision = running->capabilityRevisions.value(QStringLiteral("originGrants"));
    QPointer<ProviderRegistry> guard(this);
    if (!co_await requestNetworkConsent(accountId, scope, QStringLiteral("origin"), origin))
        throw std::runtime_error("origin_denied");
    if (!guard || m_running.value(accountId).generation != generation
        || m_running.value(accountId).networkRevision != revision)
        throw std::runtime_error("source_changed");
    if (!hasCapability(accountId, QStringLiteral("originGrants"))
        || m_running.value(accountId).capabilityRevisions.value(QStringLiteral("originGrants")) != capabilityRevision)
        throw std::runtime_error("unsupported_capability");
    ScriptRuntime *runtime = m_modules.value(m_running.value(accountId).module).runtime;
    // Worker installation is provisional: neither source nor operation hosts
    // can use these origins until the owning context and offer survive the
    // return trip. Approval and persistence then commit without a suspension.
    auto approval = std::make_shared<std::atomic_bool>(false);
    co_await runtime->grantOrigins(runtimeSourceId(accountId), { origin }, approval);
    if (!guard || m_running.value(accountId).generation != generation
        || m_running.value(accountId).networkRevision != revision)
        throw std::runtime_error("source_changed");
    if (!hasCapability(accountId, QStringLiteral("originGrants"))
        || m_running.value(accountId).capabilityRevisions.value(QStringLiteral("originGrants")) != capabilityRevision)
        throw std::runtime_error("unsupported_capability");
    ProviderAccount *entry = account(accountId);
    if (!entry)
        throw std::runtime_error("source_changed");
    approval->store(true);
    if (!entry->origins.contains(origin))
        entry->origins.append(origin);
    if (!m_running[accountId].origins.contains(origin))
        m_running[accountId].origins.append(origin);
    persist();
}

QCoro::Task<void> ProviderRegistry::allowLanDiscovery(QString draftId, QString scope)
{
    const auto running = m_running.constFind(draftId);
    if (running == m_running.cend() || !running->draft
        || !running->declaredCapabilities.value(QStringLiteral("lanProbe")).toBool())
        throw std::runtime_error("unsupported_capability");
    if (running->lanConsent)
        co_return;
    const quint64 generation = running->generation;
    const quint64 revision = running->networkRevision;
    QPointer<ProviderRegistry> guard(this);
    if (!co_await requestNetworkConsent(draftId, scope, QStringLiteral("lan")))
        throw std::runtime_error("discovery_denied");
    if (!guard || m_running.value(draftId).generation != generation
        || m_running.value(draftId).networkRevision != revision)
        throw std::runtime_error("source_changed");
    ScriptRuntime *runtime = m_modules.value(m_running.value(draftId).module).runtime;
    co_await runtime->allowLanDiscovery(draftId);
    if (!guard || m_running.value(draftId).generation != generation
        || m_running.value(draftId).networkRevision != revision)
        throw std::runtime_error("source_changed");
    m_running[draftId].lanConsent = true;
}

void ProviderRegistry::cancelLanDiscovery(const QString& draftId, const QString& scope)
{
    auto running = m_running.find(draftId);
    if (running == m_running.end() || !running->draft)
        return;
    running->lanConsent = false;
    cancelSourceScope(draftId, scope);
    if (ScriptRuntime *runtime = m_modules.value(running->module).runtime)
        runtime->cancelLanDiscovery(draftId);
}

QString ProviderRegistry::finishSetup(const QString& draftId, const QVariantMap& result)
{
    const Running draft = m_running.value(draftId);
    const QString moduleId = draft.draft ? draft.module : result.value(QStringLiteral("module")).toString();
    const QString key = result.value(QStringLiteral("account")).toString().left(256);
    if (!m_modules.contains(moduleId) || key.isEmpty() || (!draft.draft && !draftId.isEmpty()))
        return {};
    const QVariantMap context = draft.setupConfiguration.value(QStringLiteral("setupContext")).toMap();
    if (context.value(QStringLiteral("purpose")).toString() == QStringLiteral("reconnect")) {
        const ProviderAccount *target = account(context.value(QStringLiteral("accountId")).toString());
        if (!target || target->module != moduleId || target->key != key
            || target->group != result.value(QStringLiteral("group")).toString()) {
            emit problem(QStringLiteral("Sign in as the saved profile on its original server."));
            return {};
        }
    }
    const auto existing = std::find_if(m_accounts.begin(), m_accounts.end(),
        [&](const auto& account) { return account.module == moduleId && account.key == key; });
    ProviderAccount candidate = existing == m_accounts.end() ? ProviderAccount {} : *existing;
    if (candidate.id.isEmpty())
        candidate.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    candidate.module = moduleId;
    candidate.key = key;
    candidate.group = result.value(QStringLiteral("group")).toString().left(256);
    candidate.label = result.value(QStringLiteral("label")).toString().left(128);
    candidate.detail = result.value(QStringLiteral("detail")).toString().left(256);
    candidate.configuration = result.value(QStringLiteral("configuration")).toMap();
    candidate.configuration.insert(draft.pendingConfiguration);
    candidate.origins = draft.origins;
    for (const QString& origin : m_modules[moduleId].manifest.origins)
        candidate.origins.removeAll(QUrl(origin));
    const QString id = candidate.id;
    if (existing == m_accounts.end()) {
        m_onboarding.insert(id);
        ProviderAccount pending = candidate;
        pending.configuration.clear();
        pending.enabled = false;
        m_accounts.push_back(std::move(pending));
    }
    emit accountSetupStarted(id);
    Async::runScoped(
        this, start(id, draft.draft ? QStringLiteral("linked") : QStringLiteral("switch"), true, candidate), [] { },
        [](const std::exception_ptr&) { }, "provider link");
    return id;
}

void ProviderRegistry::updateConfiguration(const QString& accountId, const QVariantMap& changes)
{
    ProviderAccount *entry = account(accountId);
    if (!entry || !m_running.value(accountId).provider)
        return;
    entry->configuration.insert(changes);
    persist(true);
}

void ProviderRegistry::restartAccount(const QString& accountId, const QVariantMap& changes)
{
    if (ProviderAccount *entry = account(accountId); entry && !entry->enabled) {
        const ProviderModule *owner = module(entry->module);
        if (owner && owner->native) {
            entry->configuration.insert(changes);
            persist(true);
            emit accountsChanged();
        }
        return;
    }
    if (const ProviderAccount *entry = account(accountId); entry && entry->enabled) {
        ProviderAccount candidate = *entry;
        candidate.configuration.insert(changes);
        clearGrants(entry->module, entry->activationFamily);
        Async::runScoped(
            this, start(accountId, QStringLiteral("restart"), false, candidate), [] { },
            [](const std::exception_ptr&) { }, "provider restart");
    }
}

void ProviderRegistry::endContext(const QString& sourceId)
{
    const auto running = m_running.constFind(sourceId);
    if (running == m_running.cend() || !running->draft)
        return;
    stop(sourceId);
}

QObject *ProviderRegistry::openSettings(const QString& accountId)
{
    const ProviderAccount *entry = account(accountId);
    const ProviderModule *owner = entry ? module(entry->module) : nullptr;
    if (!entry || (!m_running.value(accountId).provider && !(owner && owner->native)))
        return nullptr;
    auto *context = createContext(accountId, QStringLiteral("settings"), entry->module);
    if (context && owner && owner->native)
        context->setArguments({ { QStringLiteral("configuration"), entry->configuration } });
    return context;
}

QObject *ProviderRegistry::openPicker(const QString& accountId, const QVariantMap& arguments)
{
    const ProviderAccount *entry = account(accountId);
    ProviderUiContext *context = entry && m_running.value(accountId).provider
        ? createContext(accountId, QStringLiteral("picker"), entry->module)
        : nullptr;
    if (context)
        context->setArguments(arguments);
    return context;
}

QCoro::Task<ProviderRegistry::PickerResult> ProviderRegistry::pickResult(
    QString sourceId, QVariantMap arguments, bool activation, QString scope)
{
    ProviderUiContext *context = nullptr;
    if (activation) {
        const auto state = m_running.constFind(sourceId);
        if (state != m_running.cend() && !state->draft && !state->provider
            && m_preparing.value(state->accountId) == sourceId) {
            context = createContext(sourceId, QStringLiteral("picker"), state->module);
            if (context)
                context->setArguments(arguments);
        }
    } else {
        context = qobject_cast<ProviderUiContext *>(openPicker(sourceId, arguments));
    }
    if (!context)
        throw std::runtime_error("picker_unavailable");
    if (!scope.isEmpty()) {
        connect(this, &ProviderRegistry::sourceScopeCancelled, context,
            [context, sourceId, scope](const QString& cancelledSource, const QString& cancelledScope) {
                if (sourceId == cancelledSource && (cancelledScope.isEmpty() || cancelledScope == scope))
                    context->close();
            });
    }
    QTimer::singleShot(0, context, [this, context] {
        if (!context->closed())
            emit componentRequested(context);
    });
    const auto [result, cancelled] = co_await qCoro(context, &ProviderUiContext::finished);
    if (activation && !cancelled)
        ProviderExtensionData::validateValue(result, 16 * 1024, "invalid_activation");
    co_return PickerResult { !cancelled, cancelled ? QVariantMap {} : result };
}

QCoro::Task<QVariantMap> ProviderRegistry::pick(QString accountId, QVariantMap arguments, QString scope)
{
    const auto result = co_await pickResult(accountId, arguments, false, scope);
    co_return result.submitted ? result.values : QVariantMap {};
}

void ProviderRegistry::useAccount(const QString& accountId)
{
    const auto *chosen = account(accountId);
    if (!chosen)
        return;
    if (m_expired.contains(accountId)) {
        emit problem(QStringLiteral("Sign in to %1 again").arg(chosen->label));
        emit accountSelectionFinished(accountId, false);
        return;
    }
    if (m_running.value(accountId).provider) {
        ProviderAccount selected = *chosen;
        selected.enabled = true;
        selected.lastUsed = QDateTime::currentMSecsSinceEpoch();
        for (const auto& other : m_accounts)
            selected.lastUsed = std::max(selected.lastUsed, other.lastUsed + 1);
        commitSelection(selected);
        if (auto *entry = account(accountId))
            *entry = selected;
        persist();
        emit accountsChanged();
        emit accountSelectionFinished(accountId, true);
        return;
    }
    Async::runScoped(
        this, start(accountId, QStringLiteral("switch"), true), [] { }, [](const std::exception_ptr&) { },
        "provider activation");
}

void ProviderRegistry::setAccountEnabled(const QString& accountId, bool enabled)
{
    if (enabled) {
        useAccount(accountId);
        return;
    }
    if (ProviderAccount *entry = account(accountId)) {
        entry->enabled = false;
        clearGrants(entry->module, entry->activationFamily);
        if (!entry->activationFamily.isEmpty() && m_running.value(accountId).provider)
            emit accountIdentityRevoked(accountId);
        stop(accountId);
        persist();
        emit accountsChanged();
    }
}

void ProviderRegistry::removeAccount(const QString& accountId)
{
    if (!account(accountId) || m_removingAccounts.contains(accountId))
        return;
    m_removingAccounts.insert(accountId);
    emit accountsChanged();
    const auto forget = [this, accountId] {
        if (const auto *entry = account(accountId)) {
            clearGrants(entry->module, entry->activationFamily);
            if (!entry->activationFamily.isEmpty() && m_running.value(accountId).provider)
                emit accountIdentityRevoked(accountId);
        }
        stop(accountId);
        if (const auto *removed = account(accountId)) {
            const ProviderAccount gone = *removed;
            const QString set = profileSet(gone);
            std::erase_if(m_accounts, [&](const ProviderAccount& account) { return account.id == accountId; });
            // A pinned startup profile moves to the same person on another
            // server; otherwise the set falls back to its last-used viewer.
            auto choice = m_startupChoices.value(set).toMap();
            const auto other = std::find_if(m_accounts.cbegin(), m_accounts.cend(),
                [&](const ProviderAccount& entry) { return sameProfile(entry, gone); });
            if (choice.value(QStringLiteral("account")).toString() == accountId && other == m_accounts.cend()) {
                m_startupChoices.remove(set);
            } else if (choice.value(QStringLiteral("account")).toString() == accountId) {
                choice.insert(QStringLiteral("account"), other->id);
                m_startupChoices.insert(set, choice);
            }
            if (std::none_of(m_accounts.cbegin(), m_accounts.cend(),
                    [&](const ProviderAccount& entry) { return profileSet(entry) == set; })) {
                m_startupChoices.remove(set);
                m_awaitingChoice.remove(set);
            }
        }
        m_failedAccounts.remove(accountId);
        m_accountErrors.remove(accountId);
        m_lockedAccounts.remove(accountId);
        m_expired.remove(accountId);
        m_onboarding.remove(accountId);
        m_unavailableConfigurations.remove(accountId);
        m_removingAccounts.remove(accountId);
        m_removedAccounts.append(accountId);
        persist(true);
        emit accountsChanged();
    };
    if (!m_running.value(accountId).provider)
        return forget();
    // The local removal must settle even when the server never replies.
    QTimer::singleShot(3000, this, [this, accountId, forget] {
        if (m_removingAccounts.contains(accountId))
            forget();
    });
    // Best effort: let the server end the session too before it is dropped.
    Async::runScoped(
        this, callSource(accountId, QStringLiteral("signOut")),
        [this, accountId, forget](QVariantMap) {
            if (m_removingAccounts.contains(accountId))
                forget();
        },
        [this, accountId, forget](const std::exception_ptr&) {
            if (m_removingAccounts.contains(accountId))
                forget();
        },
        "provider sign out");
}

void ProviderRegistry::cancelActivation(const QString& accountId)
{
    const QString prepared = m_preparing.value(accountId);
    if (!prepared.isEmpty())
        stopPublished(prepared);
}

bool ProviderRegistry::setStartupChoice(const QString& accountId, const QString& mode)
{
    const ProviderAccount *entry = account(accountId);
    // Only the authorized, active viewer can become the one opened unasked.
    if (!entry || (mode != QStringLiteral("ask") && mode != QStringLiteral("always"))
        || (mode == QStringLiteral("always")
            && (!entry->enabled || !m_running.value(accountId).provider || m_expired.contains(accountId)
                || m_removingAccounts.contains(accountId))))
        return false;
    QVariantMap choice { { QStringLiteral("mode"), mode } };
    if (mode == QStringLiteral("always"))
        choice.insert(QStringLiteral("account"), accountId);
    m_startupChoices.insert(profileSet(*entry), choice);
    m_onboarding.remove(accountId);
    persist();
    emit accountsChanged();
    return true;
}

void ProviderRegistry::finishOnboarding(const QString& accountId)
{
    if (m_onboarding.remove(accountId))
        emit accountsChanged();
}

QVariantMap ProviderRegistry::activationConfiguration(const QString& accountId) const
{
    if (!hasCapability(accountId, activationCapability))
        return {};
    const auto entry = std::find_if(m_accounts.cbegin(), m_accounts.cend(),
        [&](const ProviderAccount& account) { return account.id == accountId; });
    if (entry == m_accounts.cend() || entry->activationFamily.isEmpty())
        return {};
    return m_activationOptions.value(familyKey(*entry)).toMap();
}

} // namespace Spool
