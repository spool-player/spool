#include "ProviderRegistry.h"

#include "../cache/DatabaseManager.h"
#include "../common/AsyncTask.h"
#include "../platform/CredentialStore.h"
#include "PortableProvider.h"
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
#include <QTimer>
#include <QUuid>

#include <algorithm>

namespace JellyfinNative {

namespace {
    const QString kAccountsKey = QStringLiteral("providers/accounts/2");

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
        };
        return names;
    }

    Provider::Capabilities capabilitiesOf(const ProviderManifest& manifest)
    {
        Provider::Capabilities flags;
        for (const QString& name : manifest.capabilities)
            flags |= capabilityNames().value(name, Provider::Capability {});
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

    QJsonObject toJson(const ProviderAccount& account)
    {
        QJsonArray origins;
        for (const QUrl& origin : account.origins)
            origins.append(origin.toString());
        return { { QStringLiteral("id"), account.id }, { QStringLiteral("module"), account.module },
            { QStringLiteral("key"), account.key }, { QStringLiteral("group"), account.group },
            { QStringLiteral("label"), account.label }, { QStringLiteral("detail"), account.detail },
            { QStringLiteral("enabled"), account.enabled }, { QStringLiteral("origins"), origins },
            { QStringLiteral("lastUsed"), account.lastUsed } };
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
        return account;
    }
} // namespace

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
    const auto installed = ProviderPackage::installedVersions(m_installDirectory);
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

void ProviderRegistry::addNativeModule(ProviderManifest manifest, ProviderModule::NativeFactory factory)
{
    ProviderModule module;
    module.manifest = std::move(manifest);
    module.native = std::move(factory);
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
        module.runtime
            = new ScriptRuntime(entry.isLocalFile() ? entry.toLocalFile() : entry.toString(), m_device, m_hooks, this);
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
    std::vector<ProviderAccount> accounts;
    for (const QJsonValue& row : root.value(QStringLiteral("accounts")).toArray()) {
        ProviderAccount account = fromJson(row.toObject());
        if (!QUuid(account.id).isNull() && m_modules.contains(account.module))
            accounts.push_back(std::move(account));
    }
    // Configuration holds credentials, so it lives in the platform's
    // credential store; the keychain can block, so it is read off-thread.
    accounts = co_await Async::background(
        [accounts]() mutable {
            for (ProviderAccount& account : accounts)
                account.configuration
                    = QJsonDocument::fromJson(CredentialStore::load(account.id).toUtf8()).object().toVariantMap();
            return accounts;
        },
        &m_credentialPool);
    if (!guard)
        co_return;
    m_accounts = std::move(accounts);
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
    m_restored = true;
    emit restoredChanged();
    emit accountsChanged();
    for (const ProviderAccount& account : m_accounts) {
        if (account.enabled)
            Async::runScoped(this, start(account.id), [] { }, [](const std::exception_ptr&) { }, "provider start");
    }
}

QCoro::Task<void> ProviderRegistry::start(QString accountId)
{
    ProviderAccount *entry = account(accountId);
    if (!entry || m_running.contains(accountId) || !m_modules.contains(entry->module))
        co_return;
    ProviderModule& module = m_modules[entry->module];
    const quint64 generation = ++m_nextGeneration;
    if (module.native) {
        Provider *provider = module.native(accountId, entry->configuration, this);
        m_running.insert(accountId, { module.manifest.id, generation, provider, {}, false });
        emit sourceStarted(provider);
        emit accountsChanged();
        co_return;
    }
    QList<QUrl> origins = entry->origins;
    for (const QString& origin : module.manifest.origins)
        origins.append(QUrl(origin));
    m_running.insert(accountId, { module.manifest.id, generation, nullptr, origins, false });
    const QString label = entry->label;
    const Provider::Capabilities capabilities = capabilitiesOf(module.manifest);
    // An install or restart may replace the module's runtime while this
    // waits; the generation says whether this start is still the current one.
    QPointer<ScriptRuntime> runtime = runtimeFor(module);
    QPointer<ProviderRegistry> guard(this);
    const auto current = [&] { return guard && runtime && m_running.value(accountId).generation == generation; };
    QVariantMap description;
    try {
        co_await runtime->addSource(accountId, entry->configuration, origins);
        if (!current())
            co_return;
        description = co_await runtime->call(accountId, QStringLiteral("describe"));
    } catch (const std::exception& error) {
        if (!current())
            co_return;
        qWarning("providers: %s did not start: %s", qPrintable(accountId), error.what());
        stop(accountId);
        emit problem(QStringLiteral("Couldn't connect to %1").arg(label));
        co_return;
    }
    if (!current())
        co_return;
    auto *provider = new PortableProvider(this, accountId, label, capabilities, description, this);
    m_running[accountId].provider = provider;
    emit sourceStarted(provider);
    emit accountsChanged();
}

void ProviderRegistry::stop(const QString& accountId)
{
    const auto running = m_running.find(accountId);
    if (running == m_running.end())
        return;
    const Running state = running.value();
    m_running.erase(running);
    if (ScriptRuntime *runtime = m_modules.value(state.module).runtime)
        runtime->removeSource(accountId);
    if (state.provider) {
        emit sourceStopped(accountId);
        state.provider->shutdown();
        state.provider->deleteLater();
    }
}

void ProviderRegistry::restartModule(const QString& moduleId)
{
    for (const QString& id : m_running.keys()) {
        if (m_running.value(id).module == moduleId)
            stop(id);
    }
    ProviderModule& module = m_modules[moduleId];
    delete std::exchange(module.runtime, nullptr);
    module.failed = false;
    for (const ProviderAccount& account : m_accounts) {
        if (account.enabled && account.module == moduleId)
            Async::runScoped(this, start(account.id), [] { }, [](const std::exception_ptr&) { }, "provider restart");
    }
}

void ProviderRegistry::persist(bool credentials)
{
    QJsonArray rows;
    QHash<QString, QString> secrets;
    for (const ProviderAccount& account : m_accounts) {
        rows.append(toJson(account));
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
            QJsonDocument(QJsonObject { { QStringLiteral("format"), 2 }, { QStringLiteral("accounts"), rows } })
                .toJson(QJsonDocument::Compact)));
}

QCoro::Task<void> ProviderRegistry::install(ProviderPackageContents package)
{
    const QString root = m_installDirectory;
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
    const ProviderModule *existing = module(moduleId);
    if (!existing || (existing->bundled && !existing->overridesBundled) || existing->native)
        co_return;
    for (const QString& id : m_running.keys()) {
        if (m_running.value(id).module == moduleId)
            stop(id);
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

template <typename T, typename Call> QCoro::Task<T> ProviderRegistry::guarded(QString sourceId, Call call)
{
    const auto running = m_running.constFind(sourceId);
    ScriptRuntime *runtime = running == m_running.cend() ? nullptr : m_modules.value(running->module).runtime;
    if (!runtime)
        throw std::runtime_error("source_unavailable");
    const quint64 generation = running->generation;
    QPointer<ProviderRegistry> guard(this);
    std::optional<T> result;
    try {
        result = co_await call(runtime);
    } catch (const std::exception& error) {
        // The server no longer accepts this account's credentials: say so
        // once, and let the accounts page offer to sign in again.
        if (guard && QByteArray(error.what()) == "http_401" && !m_expired.contains(sourceId)) {
            m_expired.insert(sourceId);
            if (const ProviderAccount *entry = account(sourceId))
                emit problem(QStringLiteral("Sign in to %1 again").arg(entry->label));
            emit accountsChanged();
        }
        throw;
    }
    if (!guard || m_running.value(sourceId).generation != generation)
        throw std::runtime_error("source_changed");
    co_return std::move(*result);
}

QCoro::Task<QVariantMap> ProviderRegistry::callSource(
    QString sourceId, QString operation, QVariantMap arguments, QString scope)
{
    return guarded<QVariantMap>(
        sourceId, [=](ScriptRuntime *runtime) { return runtime->call(sourceId, operation, arguments, scope); });
}

QCoro::Task<ProviderMediaPage> ProviderRegistry::callSourceMediaPage(
    QString sourceId, QString operation, QVariantMap arguments, int maximumItems)
{
    return guarded<ProviderMediaPage>(sourceId, [=](ScriptRuntime *runtime) {
        return runtime->callMediaPage(sourceId, operation, arguments, {}, maximumItems);
    });
}

QCoro::Task<MovieItem> ProviderRegistry::callSourceItem(QString sourceId, QString operation, QVariantMap arguments)
{
    return guarded<MovieItem>(
        sourceId, [=](ScriptRuntime *runtime) { return runtime->callItem(sourceId, operation, arguments); });
}

void ProviderRegistry::cancelSourceScope(const QString& sourceId, const QString& scope)
{
    const auto running = m_running.constFind(sourceId);
    if (running != m_running.cend()) {
        if (ScriptRuntime *runtime = m_modules.value(running->module).runtime)
            runtime->cancelScope(sourceId, scope);
    }
}

bool ProviderRegistry::sourceRunning(const QString& sourceId) const
{
    return m_running.contains(sourceId);
}

void ProviderRegistry::handleEvent(const QString& sourceId, const QString& type, const QVariantMap& payload)
{
    const Running running = m_running.value(sourceId);
    if (type == QStringLiteral("configuration")) {
        // A refreshed token or a moved server; the running source already
        // has it, so this only has to survive the next launch.
        if (!running.draft)
            updateConfiguration(sourceId, payload);
        return;
    }
    if (!running.provider)
        return;
    if (type == QStringLiteral("changed"))
        emit running.provider->contentChanged(payload.value(QStringLiteral("itemId")).toString());
    else
        emit running.provider->sourceEvent(type, payload);
}

void ProviderRegistry::handleInterrupted(const QString& moduleId)
{
    ProviderModule& module = m_modules[moduleId];
    module.failed = true;
    for (const QString& id : m_running.keys()) {
        if (m_running.value(id).module == moduleId)
            stop(id);
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
        list.append(QVariantMap { { QStringLiteral("id"), account.id }, { QStringLiteral("moduleId"), account.module },
            { QStringLiteral("providerName"), owner ? owner->manifest.name : account.module },
            { QStringLiteral("iconUrl"), owner ? owner->file(owner->manifest.icon) : QUrl() },
            { QStringLiteral("label"), account.label }, { QStringLiteral("detail"), account.detail },
            { QStringLiteral("group"), account.group }, { QStringLiteral("enabled"), account.enabled },
            { QStringLiteral("running"), bool(m_running.value(account.id).provider) },
            { QStringLiteral("needsSignIn"), m_expired.contains(account.id) },
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
    connect(this, &ProviderRegistry::sourceStopped, context, [context](const QString& id) {
        if (id == context->sourceId())
            context->close();
    });
    return context;
}

QObject *ProviderRegistry::beginSetup(const QString& moduleId)
{
    ProviderModule *owner = m_modules.contains(moduleId) ? &m_modules[moduleId] : nullptr;
    if (!owner)
        return nullptr;
    if (!owner->manifest.needsAccount()) {
        const auto existing = std::find_if(
            m_accounts.begin(), m_accounts.end(), [&](const auto& account) { return account.module == moduleId; });
        const QString id = existing != m_accounts.end()
            ? existing->id
            : finishSetup({},
                  { { QStringLiteral("module"), moduleId }, { QStringLiteral("account"), QStringLiteral("default") },
                      { QStringLiteral("label"), owner->manifest.name } });
        useAccount(id);
        emit accountAdded(id);
        return nullptr;
    }
    const QString draftId = QStringLiteral("setup-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QList<QUrl> origins;
    for (const QString& origin : owner->manifest.origins)
        origins.append(QUrl(origin));
    m_running.insert(draftId, { moduleId, ++m_nextGeneration, nullptr, origins, true });
    Async::runScoped(
        this, runtimeFor(*owner)->addSource(draftId, {}, origins), [](QVariantMap) {},
        [this, draftId](const std::exception_ptr&) {
            m_running.remove(draftId);
            emit problem(QStringLiteral("This provider could not start"));
        },
        "provider setup");
    return createContext(draftId, QStringLiteral("login"), moduleId);
}

QCoro::Task<void> ProviderRegistry::allowSetupOrigin(QString draftId, QUrl url)
{
    const auto running = m_running.find(draftId);
    if (running == m_running.end() || !running->draft || url.host().isEmpty()
        || (url.scheme() != QStringLiteral("http") && url.scheme() != QStringLiteral("https")))
        throw std::runtime_error("origin_denied");
    const QUrl origin = originOf(url);
    if (running->origins.contains(origin))
        co_return;
    running->origins.append(origin);
    running->generation = ++m_nextGeneration;
    ScriptRuntime *runtime = m_modules.value(running->module).runtime;
    // Origins are fixed when a source is created; a setup source keeps no
    // state worth saving, so recreate it with the server the viewer named.
    runtime->removeSource(draftId);
    co_await runtime->addSource(draftId, {}, running->origins);
}

QString ProviderRegistry::finishSetup(const QString& draftId, const QVariantMap& result)
{
    const Running draft = m_running.value(draftId);
    const QString moduleId = draft.draft ? draft.module : result.value(QStringLiteral("module")).toString();
    const QString key = result.value(QStringLiteral("account")).toString().left(256);
    if (!m_modules.contains(moduleId) || key.isEmpty())
        return {};
    auto existing = std::find_if(m_accounts.begin(), m_accounts.end(),
        [&](const auto& account) { return account.module == moduleId && account.key == key; });
    if (existing == m_accounts.end()) {
        ProviderAccount created;
        created.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        created.module = moduleId;
        created.key = key;
        m_accounts.push_back(std::move(created));
        existing = std::prev(m_accounts.end());
    }
    existing->group = result.value(QStringLiteral("group")).toString().left(256);
    existing->label = result.value(QStringLiteral("label")).toString().left(128);
    existing->detail = result.value(QStringLiteral("detail")).toString().left(256);
    existing->configuration = result.value(QStringLiteral("configuration")).toMap();
    // Origins come from what the viewer typed or picked, never from the
    // provider's own claim about where it should be allowed to go.
    existing->origins = draft.origins;
    for (const QString& origin : m_modules[moduleId].manifest.origins)
        existing->origins.removeAll(QUrl(origin));
    const QString id = existing->id;
    m_expired.remove(id);
    stop(id);
    persist(true);
    if (draft.draft) {
        useAccount(id);
        emit accountAdded(id);
    }
    return id;
}

void ProviderRegistry::updateConfiguration(const QString& accountId, const QVariantMap& changes)
{
    ProviderAccount *entry = account(accountId);
    if (!entry)
        return;
    entry->configuration.insert(changes);
    persist(true);
}

void ProviderRegistry::restartAccount(const QString& accountId)
{
    stop(accountId);
    if (const ProviderAccount *entry = account(accountId); entry && entry->enabled)
        Async::runScoped(this, start(accountId), [] { }, [](const std::exception_ptr&) { }, "provider restart");
}

void ProviderRegistry::endContext(const QString& sourceId)
{
    const auto running = m_running.constFind(sourceId);
    if (running == m_running.cend() || !running->draft)
        return;
    if (ScriptRuntime *runtime = m_modules.value(running->module).runtime)
        runtime->removeSource(sourceId);
    m_running.remove(sourceId);
}

QObject *ProviderRegistry::openSettings(const QString& accountId)
{
    const ProviderAccount *entry = account(accountId);
    return entry && m_running.value(accountId).provider
        ? createContext(accountId, QStringLiteral("settings"), entry->module)
        : nullptr;
}

QObject *ProviderRegistry::openPicker(const QString& accountId, const QVariantMap& arguments)
{
    const ProviderAccount *entry = account(accountId);
    ProviderUiContext *context = entry ? createContext(accountId, QStringLiteral("picker"), entry->module) : nullptr;
    if (context)
        context->setArguments(arguments);
    return context;
}

QCoro::Task<QVariantMap> ProviderRegistry::pick(QString accountId, QVariantMap arguments)
{
    auto *context = qobject_cast<ProviderUiContext *>(openPicker(accountId, arguments));
    if (!context)
        throw std::runtime_error("picker_unavailable");
    // Mounted on the next turn, once this is waiting: a screen may finish as
    // soon as it is shown.
    QTimer::singleShot(0, context, [this, context] { emit componentRequested(context); });
    const auto [result, cancelled] = co_await qCoro(context, &ProviderUiContext::finished);
    co_return cancelled ? QVariantMap {} : result;
}

void ProviderRegistry::useAccount(const QString& accountId)
{
    ProviderAccount *chosen = account(accountId);
    if (!chosen)
        return;
    chosen->enabled = true;
    chosen->lastUsed = QDateTime::currentMSecsSinceEpoch();
    const QString module = chosen->module;
    const QString group = chosen->group;
    for (ProviderAccount& other : m_accounts) {
        if (other.id != accountId && !group.isEmpty() && other.module == module && other.group == group
            && other.enabled) {
            other.enabled = false;
            stop(other.id);
        }
    }
    persist();
    emit accountsChanged();
    Async::runScoped(this, start(accountId), [] { }, [](const std::exception_ptr&) { }, "provider start");
}

void ProviderRegistry::setAccountEnabled(const QString& accountId, bool enabled)
{
    if (enabled) {
        useAccount(accountId);
        return;
    }
    if (ProviderAccount *entry = account(accountId)) {
        entry->enabled = false;
        stop(accountId);
        persist();
        emit accountsChanged();
    }
}

void ProviderRegistry::removeAccount(const QString& accountId)
{
    const auto forget = [this, accountId] {
        stop(accountId);
        std::erase_if(m_accounts, [&](const ProviderAccount& account) { return account.id == accountId; });
        m_removedAccounts.append(accountId);
        persist(true);
        emit accountsChanged();
    };
    if (!m_running.value(accountId).provider)
        return forget();
    // Best effort: let the server end the session too before it is dropped.
    Async::runScoped(
        this, callSource(accountId, QStringLiteral("signOut")), [forget](QVariantMap) { forget(); },
        [forget](const std::exception_ptr&) { forget(); }, "provider sign out");
}

} // namespace JellyfinNative
