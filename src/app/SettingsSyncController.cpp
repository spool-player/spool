#include "SettingsSyncController.h"

#include "../cache/DatabaseManager.h"
#include "../common/AsyncTask.h"
#include "../provider/ProviderRegistry.h"
#include "SettingsController.h"
#include "SettingsSchema.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Spool {
namespace {
    namespace Doc = SettingsSyncDocument;
    const QString Preferences = QStringLiteral("playbackPreferences");
    const QString Storage = QStringLiteral("settingsStorage");
    const QString DocumentId = QStringLiteral("278fca80-aaf9-4d32-8458-388836790234");
    const QString Scope = QStringLiteral("settings-sync");
    QString stateKey(const QString& account)
    {
        return QStringLiteral("settingsSync/state/") + account;
    }
    QString json(const QVariantMap& value)
    {
        return QString::fromUtf8(QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact));
    }
    QVariantMap parseMap(const QString& text, int maximum = 256 * 1024)
    {
        if (text.isEmpty())
            return {};
        const auto bytes = text.toUtf8();
        if (bytes.size() > maximum)
            throw std::runtime_error("oversized_local_sync_state");
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            throw std::runtime_error("invalid_local_sync_state");
        return document.object().toVariantMap();
    }
    QVariantMap entryMap(const Doc::Entry& entry)
    {
        return { { QStringLiteral("value"), entry.value }, { QStringLiteral("clock"), entry.clock },
            { QStringLiteral("nonce"), entry.nonce } };
    }
    Doc::Entry mapEntry(const QVariantMap& value)
    {
        return { value.value(QStringLiteral("value")), value.value(QStringLiteral("clock")).toString(),
            value.value(QStringLiteral("nonce")).toString() };
    }
    bool offlineProblem(const QString& problem)
    {
        return problem == QStringLiteral("network_error") || problem == QStringLiteral("network_offline")
            || problem == QStringLiteral("operation_timeout") || problem == QStringLiteral("source_unavailable");
    }
}

SettingsSyncController::SettingsSyncController(
    SettingsController *settings, DatabaseManager *database, ProviderRegistry *registry, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_database(database)
    , m_registry(registry)
{
    m_clock.start();
    m_debounce.setSingleShot(true);
    connect(&m_debounce, &QTimer::timeout, this, &SettingsSyncController::startCycle);
    m_refreshTimer.setInterval(60000);
    connect(&m_refreshTimer, &QTimer::timeout, this, &SettingsSyncController::refresh);
    connect(settings, &SettingsController::userValuesCommitted, this, &SettingsSyncController::committed);
    connect(settings, &SettingsController::settingsPersistenceFailed, this, &SettingsSyncController::persistenceFailed);
    connect(settings, &SettingsController::userValuesCommitFailed, this, [this] {
        if (m_pendingLocalCommits > 0)
            --m_pendingLocalCommits;
    });
    connect(registry, &ProviderRegistry::accountsChanged, this, &SettingsSyncController::reconcileAccounts);
    connect(registry, &ProviderRegistry::restoredChanged, this, &SettingsSyncController::reconcileAccounts);
    connect(registry, &ProviderRegistry::capabilitiesChanged, this, [this](const QString& account) {
        if (account == m_accountId) {
            invalidate();
            m_settings->cancelRemoteApplications({}, false);
            m_nativeKnown = false;
            m_nativeWritable.clear();
            m_confirmed.clear();
        }
        reconcileAccounts();
        if (account == m_accountId)
            schedule();
    });
    connect(registry, &ProviderRegistry::sourceStopped, this, [this](const QString& account) {
        if (account == m_accountId) {
            invalidate();
            m_settings->cancelRemoteApplications({}, false);
            m_lastConnectionState.clear();
            m_nativeKnown = false;
            m_nativeWritable.clear();
            m_confirmed.clear();
            emit changed();
        }
    });
}

SettingsSyncController::~SettingsSyncController()
{
    m_registry->cancelSourceScope(m_accountId, Scope);
}

QCoro::Task<void> SettingsSyncController::loadLocalAsync()
{
    QPointer<SettingsSyncController> guard(this);
    const auto controls = co_await m_database->loadValuesAsync({ QStringLiteral("settingsSync/enabled"),
        QStringLiteral("settingsSync/accountId"), QStringLiteral("settingsSync/overrides") });
    if (!guard)
        co_return;
    m_enabled = controls.value(QStringLiteral("settingsSync/enabled"), QStringLiteral("true")).toString()
        != QStringLiteral("false");
    m_accountId = controls.value(QStringLiteral("settingsSync/accountId")).toString();
    try {
        m_overrides = parseMap(controls.value(QStringLiteral("settingsSync/overrides")).toString(), 64 * 1024);
        if (!m_accountId.isEmpty()) {
            const auto stored = co_await m_database->loadSettingAsync(stateKey(m_accountId));
            if (!guard)
                co_return;
            restoreLedger(stored);
        }
    } catch (const std::exception&) {
        if (!guard)
            co_return;
        m_persistenceFailed = true;
        m_problem = tr("Saved settings sync state is invalid. Turn sync off before linking again.");
    }
    m_loaded = true;
    // Recovery consumes explicit unfinished applications, never a replica/local
    // difference. In particular an off -> local locale edit -> restart is inert.
    co_await m_settings->recoverUnfinishedApplications();
    if (!guard)
        co_return;
    reconcileAccounts();
    if (m_enabled && m_foreground)
        m_refreshTimer.start();
    emit changed();
}

QVariantList SettingsSyncController::accounts() const
{
    QVariantList result;
    for (const auto& item : m_registry->accounts()) {
        auto row = item.toMap();
        const auto id = row.value(QStringLiteral("id")).toString();
        const auto *module = m_registry->module(row.value(QStringLiteral("moduleId")).toString());
        const bool declared = module
            && (module->manifest.capabilities.contains(Preferences) || module->manifest.capabilities.contains(Storage));
        row.insert(QStringLiteral("syncSupported"),
            row.value(QStringLiteral("connectionState")).toString() == QStringLiteral("active")
                ? m_registry->hasCapability(id, Preferences) || m_registry->hasCapability(id, Storage)
                : declared);
        row.insert(QStringLiteral("syncLabel"),
            row.value(QStringLiteral("providerName")).toString() + QStringLiteral(" — ")
                + row.value(QStringLiteral("label")).toString());
        result.append(row);
    }
    return result;
}

bool SettingsSyncController::accountActive() const
{
    for (const auto& item : m_registry->accounts()) {
        const auto row = item.toMap();
        if (row.value(QStringLiteral("id")).toString() == m_accountId)
            return row.value(QStringLiteral("enabled")).toBool()
                && row.value(QStringLiteral("connectionState")).toString() == QStringLiteral("active");
    }
    return false;
}

bool SettingsSyncController::keyEnabled(const QString& key, const QVariant& value) const
{
    const auto *spec = findSettingSpec(key);
    if (!spec || !settingSupportedOnPlatform(*spec))
        return false;
    const auto policy = settingSyncPolicy(*spec, value);
    if (policy == SettingSyncPolicy::Never)
        return false;
    const auto overrides = m_overrides.value(m_accountId).toMap();
    if (overrides.contains(key))
        return overrides.value(key).toBool();
    return policy == SettingSyncPolicy::PortableDefault;
}

bool SettingsSyncController::valueSupported(const QString& key, const QVariant& value) const
{
    return m_settings->supportsSyncValue(key, value);
}

bool SettingsSyncController::retainDocumentKey(const QString& key)
{
    if (const auto *spec = findSettingSpec(key))
        return spec->syncPolicy != SettingSyncPolicy::Never;
    // Unknown future settings survive round trips, but reserved stores are never
    // application settings even when a peer supplies them as document entries.
    for (const auto& prefix :
        { "settingsSync/", "settings/", "accounts/", "providers/", "credentials/", "origins/", "certificates/",
            "trust/", "tls/", "client/", "device/", "uiSession/", "session/", "cache/", "diagnostics/" })
        if (key.startsWith(QLatin1String(prefix)))
            return false;
    if (key == QStringLiteral("deviceId") || key == QStringLiteral("deviceIdentity")
        || key == QStringLiteral("providerAccounts") || key == QStringLiteral("token")
        || key == QStringLiteral("password"))
        return false;
    return true;
}

QString SettingsSyncController::backend(const QString& key) const
{
    const auto *spec = findSettingSpec(key);
    if (!spec || spec->syncPolicy == SettingSyncPolicy::Never)
        return QStringLiteral("none");
    if (spec->nativePreference[0] && !m_nativeKnown)
        return QStringLiteral("none");
    if (m_nativeKnown && spec->nativePreference[0]
        && m_nativeWritable.contains(QString::fromLatin1(spec->nativePreference)))
        return QStringLiteral("native");
    if (m_storageAvailable)
        return QStringLiteral("spool");
    return QStringLiteral("none");
}

QVariantMap SettingsSyncController::states() const
{
    QVariantMap result;
    QString label;
    for (const auto& item : accounts()) {
        const auto row = item.toMap();
        if (row.value(QStringLiteral("id")).toString() == m_accountId) {
            label = row.value(QStringLiteral("syncLabel")).toString();
            break;
        }
    }
    const bool active = accountActive();
    for (const auto& spec : settingSpecs()) {
        const auto key = QString::fromLatin1(spec.key);
        const bool eligible = spec.syncPolicy != SettingSyncPolicy::Never && settingSupportedOnPlatform(spec);
        const bool on = eligible && keyEnabled(key, m_settings->value(key));
        const auto channel = eligible ? backend(key) : QStringLiteral("none");
        const auto record = m_keys.value(key).toMap();
        const auto channelProblem = channel == QStringLiteral("native") ? m_nativeProblem : m_storageProblem;
        QString status;
        QString help;
        if (!eligible || !m_enabled || !on)
            status = QStringLiteral("off");
        else if (m_accountId.isEmpty())
            status = QStringLiteral("unsupported");
        else if (!active)
            status = QStringLiteral("offline");
        else if (!m_persistenceFailed
            && (offlineProblem(m_problem)
                || (!channelProblem.isEmpty()
                    && (channel == QStringLiteral("native") ? m_nativeOffline : m_storageOffline))))
            status = QStringLiteral("offline");
        else if (m_persistenceFailed || !m_problem.isEmpty()
            || (channel == QStringLiteral("native") && !m_nativeProblem.isEmpty())
            || (channel == QStringLiteral("spool") && !m_storageProblem.isEmpty()))
            status = QStringLiteral("error");
        else if (channel == QStringLiteral("none") && m_nativeKnown)
            status = QStringLiteral("unsupported");
        else if (m_replica.contains(key)
            && (!valueSupported(key, m_replica.value(key).value) || !keyEnabled(key, m_replica.value(key).value)))
            status = QStringLiteral("unsupported");
        else if (m_busy)
            status = QStringLiteral("saving");
        else if (!m_confirmed.contains(key) || record.value(QStringLiteral("bootstrap"), true).toBool()
            || record.contains(QStringLiteral("intent")))
            status = QStringLiteral("pending");
        else
            status = QStringLiteral("synced");
        if (channel == QStringLiteral("native"))
            help = tr("Changes the service's own preference.");
        else if (channel == QStringLiteral("spool"))
            help = tr("Spool-specific sync. Does not change the service's settings.");
        else
            help = tr("This account does not support synchronizing this setting.");
        if (!label.isEmpty())
            help += QLatin1Char(' ') + label;
        help += QLatin1Char(' ') + (status == QStringLiteral("off") ? tr("Local only.") : status);
        if (!channelProblem.isEmpty())
            help += QLatin1Char(' ') + channelProblem;
        if (!m_problem.isEmpty())
            help += QLatin1Char(' ') + m_problem;
        result.insert(key,
            QVariantMap { { QStringLiteral("eligible"), eligible }, { QStringLiteral("enabled"), on },
                { QStringLiteral("backend"), channel }, { QStringLiteral("status"), status },
                { QStringLiteral("help"), help } });
    }
    return result;
}

QString SettingsSyncController::status() const
{
    if (!m_enabled)
        return QStringLiteral("off");
    if (m_persistenceFailed)
        return QStringLiteral("saveFailed");
    if (m_accountId.isEmpty()) {
        for (const auto& item : accounts())
            if (item.toMap().value(QStringLiteral("syncSupported")).toBool()
                && item.toMap().value(QStringLiteral("connectionState")).toString() == QStringLiteral("starting"))
                return QStringLiteral("starting");
        return QStringLiteral("noAccount");
    }
    for (const auto& item : accounts()) {
        const auto row = item.toMap();
        if (row.value(QStringLiteral("id")).toString() != m_accountId)
            continue;
        if (row.value(QStringLiteral("needsSignIn")).toBool())
            return QStringLiteral("signedOut");
        const auto connection = row.value(QStringLiteral("connectionState")).toString();
        if (connection == QStringLiteral("starting"))
            return QStringLiteral("starting");
        if (connection == QStringLiteral("failed"))
            return QStringLiteral("failed");
        if (!accountActive())
            return QStringLiteral("inactive");
        if (!row.value(QStringLiteral("syncSupported")).toBool())
            return QStringLiteral("unsupported");
        if (offlineProblem(m_problem) || m_nativeOffline || m_storageOffline)
            return QStringLiteral("offline");
        if (!m_problem.isEmpty() || !m_nativeProblem.isEmpty() || !m_storageProblem.isEmpty())
            return QStringLiteral("error");
        if (busy())
            return QStringLiteral("syncing");
        for (const auto& value : m_keys)
            if (value.toMap().contains(QStringLiteral("intent")))
                return QStringLiteral("pending");
        return QStringLiteral("synced");
    }
    return QStringLiteral("removed");
}

QString SettingsSyncController::summary() const
{
    const auto state = status();
    if (state == QStringLiteral("off"))
        return tr("Settings stay on this device.");
    if (state == QStringLiteral("noAccount"))
        return tr("Choose an account that supports settings sync.");
    if (state == QStringLiteral("removed"))
        return tr("Your sync account was removed. Choose another account.");
    if (state == QStringLiteral("signedOut"))
        return tr("Sign in to your sync account again to resume.");
    if (state == QStringLiteral("starting"))
        return tr("Connecting to your sync account…");
    if (state == QStringLiteral("inactive"))
        return tr("Sync is paused while this account is not the active profile.");
    if (state == QStringLiteral("failed"))
        return tr("Cannot reach your sync account. Reconnect to resume.");
    if (state == QStringLiteral("unsupported"))
        return tr("This account does not support settings sync.");
    if (state == QStringLiteral("offline"))
        return tr("Changes are kept on this device until you are back online.");
    if (state == QStringLiteral("saveFailed"))
        return tr("Could not save settings on this device. Retry to recover.");
    if (state == QStringLiteral("error"))
        return tr("Could not sync settings. Your local settings are kept.");
    if (state == QStringLiteral("syncing"))
        return tr("Synchronizing settings…");
    if (state == QStringLiteral("pending"))
        return tr("Local changes are waiting to sync.");
    return tr("Settings sync is up to date.");
}

QString SettingsSyncController::problemDetail() const
{
    QStringList details;
    for (const auto& problem : { m_problem, m_nativeProblem, m_storageProblem })
        if (!problem.isEmpty())
            details.append(problem);
    return details.join(QLatin1Char('\n'));
}

bool SettingsSyncController::customized() const
{
    return !m_overrides.value(m_accountId).toMap().isEmpty();
}

QString SettingsSyncController::accountChangeWarning() const
{
    return tr("Changing the settings sync account discards unsent changes for the previous account. "
              "The selected account's saved settings will be loaded first. Continue?");
}

quint64 SettingsSyncController::keyGeneration(const QString& key) const
{
    return m_keys.value(key).toMap().value(QStringLiteral("generation")).toString().toULongLong();
}

QVariantMap SettingsSyncController::serializedLedger() const
{
    if (m_accountId.isEmpty())
        return {};
    // A union of individually bounded remote winners can exceed the wire
    // budget. Keep that bounded local union durably; encoding for upload then
    // reports the size error without forgetting an acknowledged winner.
    QVariantMap replica;
    for (auto it = m_replica.cbegin(); it != m_replica.cend(); ++it)
        if (retainDocumentKey(it.key()))
            replica.insert(it.key(), entryMap(*it));
    const QVariantMap state { { QStringLiteral("format"), 1 }, { QStringLiteral("counter"), m_counter },
        { QStringLiteral("keys"), m_keys },
        { QStringLiteral("replica"),
            QVariantMap { { QStringLiteral("format"), 1 }, { QStringLiteral("entries"), replica } } } };
    const auto serialized = json(state);
    if (serialized.toUtf8().size() > 256 * 1024)
        throw std::runtime_error("oversized_local_sync_state");
    return { { stateKey(m_accountId), serialized } };
}

QVariantMap SettingsSyncController::serializedControls() const
{
    return { { QStringLiteral("settingsSync/enabled"), m_enabled ? QStringLiteral("true") : QStringLiteral("false") },
        { QStringLiteral("settingsSync/accountId"), m_accountId },
        { QStringLiteral("settingsSync/overrides"), json(m_overrides) } };
}

void SettingsSyncController::restoreLedger(const QString& serialized)
{
    const auto state = parseMap(serialized);
    if (state.isEmpty())
        return;
    if (state.value(QStringLiteral("format")).toInt() != 1)
        throw std::runtime_error("invalid_local_sync_format");
    const auto counter = state.value(QStringLiteral("counter")).toString();
    const auto replica = state.value(QStringLiteral("replica")).toMap();
    const auto replicaRows = replica.value(QStringLiteral("entries")).toMap();
    if (!Doc::validClock(counter) || replica.value(QStringLiteral("format")).toInt() != 1 || replicaRows.size() > 512)
        throw std::runtime_error("invalid_local_sync_replica");
    m_counter = counter;
    for (auto it = replicaRows.cbegin(); it != replicaRows.cend(); ++it) {
        const auto decoded = Doc::decode(QVariantMap { { QStringLiteral("format"), 1 },
                                             { QStringLiteral("entries"), QVariantMap { { it.key(), it.value() } } } },
            retainDocumentKey);
        if (decoded.status != Doc::DecodeStatus::Valid)
            throw std::runtime_error("invalid_local_sync_entry");
        m_replica.insert(decoded.entries);
    }
    m_counter = Doc::maximumClock(m_replica, m_counter);
    const auto keys = state.value(QStringLiteral("keys")).toMap();
    if (keys.size() > settingSpecs().size())
        throw std::runtime_error("invalid_local_sync_keys");
    for (auto it = keys.cbegin(); it != keys.cend(); ++it) {
        const auto *spec = findSettingSpec(it.key());
        if (!spec || spec->syncPolicy == SettingSyncPolicy::Never)
            continue;
        auto row = it.value().toMap();
        const auto intent = row.value(QStringLiteral("intent")).toMap();
        if (row.contains(QStringLiteral("generation"))) {
            bool valid = false;
            row.value(QStringLiteral("generation")).toString().toULongLong(&valid);
            if (!valid)
                throw std::runtime_error("invalid_local_commit_generation");
        }
        if (intent.contains(QStringLiteral("entry"))) {
            const auto stamp = mapEntry(intent.value(QStringLiteral("entry")).toMap());
            if (!Doc::validClock(stamp.clock) || !Doc::validNonce(stamp.nonce))
                throw std::runtime_error("invalid_local_intent_stamp");
        }
        if (!intent.isEmpty() && !valueSupported(it.key(), intent.value(QStringLiteral("value"))))
            row.remove(QStringLiteral("intent"));
        if (!m_enabled || !keyEnabled(it.key(), m_settings->value(it.key()))) {
            row.remove(QStringLiteral("intent"));
            row.insert(QStringLiteral("bootstrap"), true);
            m_replica.remove(it.key());
        }
        m_keys.insert(it.key(), row);
    }
}

void SettingsSyncController::resetKey(const QString& key, bool cancelApplications)
{
    auto row = m_keys.value(key).toMap();
    row.remove(QStringLiteral("intent"));
    row.remove(QStringLiteral("policyPaused"));
    row.insert(QStringLiteral("bootstrap"), true);
    m_keys.insert(key, row);
    m_replica.remove(key);
    m_deferred.remove(key);
    m_confirmed.remove(key);
    if (cancelApplications)
        m_settings->cancelRemoteApplications(key);
}

void SettingsSyncController::invalidate()
{
    m_generation.invalidate();
    m_deferred.clear();
    m_debounce.stop();
    m_registry->cancelSourceScope(m_accountId, Scope);
}

void SettingsSyncController::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;
    invalidate();
    m_enabled = enabled;
    m_problem.clear();
    m_nativeProblem.clear();
    m_storageProblem.clear();
    m_persistenceFailed = false;
    for (const auto& spec : settingSpecs())
        if (spec.syncPolicy != SettingSyncPolicy::Never)
            resetKey(QString::fromLatin1(spec.key));
    if (enabled && m_foreground)
        m_refreshTimer.start();
    else
        m_refreshTimer.stop();
    persistControls(serializedLedger());
    emit changed();
}

void SettingsSyncController::setAccountId(const QString& id)
{
    if (id.isEmpty() || id == m_accountId)
        return;
    bool exists = false;
    for (const auto& account : m_registry->accountList())
        if (account.id == id)
            exists = true;
    if (!exists)
        return;
    if (m_accountId.isEmpty()) {
        selectAccount(id);
        return;
    }
    m_pendingAccountId = id;
    emit changed();
}

void SettingsSyncController::confirmAccountChange(bool approved)
{
    const auto id = std::exchange(m_pendingAccountId, {});
    if (approved && !id.isEmpty())
        selectAccount(id);
    emit changed();
}

void SettingsSyncController::selectAccount(const QString& id)
{
    invalidate();
    QVariantMap cleared;
    if (!m_accountId.isEmpty())
        cleared.insert(stateKey(m_accountId), QString());
    m_settings->cancelRemoteApplications();
    m_accountId = id;
    m_keys.clear();
    m_replica.clear();
    m_counter = QStringLiteral("0");
    m_confirmed.clear();
    m_deferred.clear();
    m_nativeKnown = false;
    m_nativeWritable.clear();
    m_storageAvailable = m_registry->hasCapability(id, Storage);
    m_lastConnectionState.clear();
    m_problem.clear();
    m_nativeProblem.clear();
    m_storageProblem.clear();
    m_persistenceFailed = false;
    cleared.insert(stateKey(id), QString());
    persistControls(cleared);
    emit changed();
}

void SettingsSyncController::setSettingEnabled(const QString& key, bool enabled)
{
    setSettingsEnabled({ key }, enabled);
}

void SettingsSyncController::setSettingsEnabled(const QStringList& keys, bool enabled)
{
    if (m_accountId.isEmpty())
        return;
    auto overrides = m_overrides.value(m_accountId).toMap();
    QStringList changedKeys;
    for (const auto& key : keys) {
        const auto *spec = findSettingSpec(key);
        if (!spec || spec->syncPolicy == SettingSyncPolicy::Never || !settingSupportedOnPlatform(*spec)
            || (overrides.contains(key) && overrides.value(key).toBool() == enabled))
            continue;
        overrides.insert(key, enabled);
        changedKeys.append(key);
    }
    if (changedKeys.isEmpty())
        return;
    invalidate();
    m_overrides.insert(m_accountId, overrides);
    for (const auto& key : changedKeys)
        resetKey(key);
    persistControls(serializedLedger());
    emit changed();
}

void SettingsSyncController::resetSettingOverrides()
{
    const auto overrides = m_overrides.value(m_accountId).toMap();
    if (overrides.isEmpty())
        return;
    invalidate();
    m_overrides.remove(m_accountId);
    for (auto it = overrides.cbegin(); it != overrides.cend(); ++it)
        resetKey(it.key());
    persistControls(serializedLedger());
    emit changed();
}

void SettingsSyncController::persistControls(QVariantMap extra)
{
    auto serialized = serializedControls();
    for (auto it = extra.cbegin(); it != extra.cend(); ++it)
        serialized.insert(it.key(), it.value());
    ++m_controlWrites;
    Async::runScoped(
        this, m_database->saveSettings(serialized),
        [this] {
            --m_controlWrites;
            schedule();
            emit changed();
        },
        [this](const std::exception_ptr&) {
            --m_controlWrites;
            persistenceFailed();
        },
        "settings sync controls");
}

void SettingsSyncController::reconcileAccounts()
{
    if (!m_loaded)
        return;
    if (m_accountId.isEmpty()) {
        // Registry order is persisted connection order, not name, last-used, or
        // the order in which parallel describe requests happened to complete.
        for (const auto& item : m_registry->accounts()) {
            const auto row = item.toMap();
            if (!row.value(QStringLiteral("enabled")).toBool() && !row.value(QStringLiteral("pendingEnabled")).toBool())
                continue;
            const auto state = row.value(QStringLiteral("connectionState")).toString();
            if (state == QStringLiteral("starting")) {
                const auto *module = m_registry->module(row.value(QStringLiteral("moduleId")).toString());
                if (module
                    && (module->manifest.capabilities.contains(Preferences)
                        || module->manifest.capabilities.contains(Storage)))
                    break;
                continue;
            }
            const auto id = row.value(QStringLiteral("id")).toString();
            if (state == QStringLiteral("active")
                && (m_registry->hasCapability(id, Preferences) || m_registry->hasCapability(id, Storage))) {
                selectAccount(id);
                break;
            }
        }
    } else {
        bool found = false;
        for (const auto& item : m_registry->accounts()) {
            const auto row = item.toMap();
            if (row.value(QStringLiteral("id")).toString() != m_accountId)
                continue;
            found = true;
            const auto state = row.value(QStringLiteral("connectionState")).toString();
            m_storageAvailable = m_registry->hasCapability(m_accountId, Storage);
            if (state != m_lastConnectionState) {
                invalidate();
                m_lastConnectionState = state;
                m_confirmed.clear();
                if (state != QStringLiteral("active"))
                    m_settings->cancelRemoteApplications({}, false);
                if (state == QStringLiteral("active"))
                    schedule();
            }
            break;
        }
        if (!found && m_registry->restored()) {
            invalidate();
            m_keys.clear();
            m_replica.clear();
            m_deferred.clear();
            m_confirmed.clear();
            m_settings->cancelRemoteApplications();
            // Keep a selected-account tombstone so removal cannot silently
            // import a different person's settings on this or the next launch.
            m_problem = tr("The settings sync account was removed. Choose a new sync account.");
            persistControls({ { stateKey(m_accountId), QString() } });
        }
    }
    emit changed();
}

bool SettingsSyncController::current(Token token) const
{
    return m_generation.isCurrent(token) && m_enabled && m_foreground && accountActive();
}

void SettingsSyncController::schedule(int delay, bool manual)
{
    m_requested = true;
    m_manualRequested = m_manualRequested || manual;
    if (!m_loaded || !m_enabled || !m_foreground || !accountActive() || m_persistenceFailed || m_controlWrites
        || m_pendingLocalCommits || m_busy)
        return;
    if (!m_manualRequested)
        delay = std::max(delay, int(std::max<qint64>(0, 30000 - (m_clock.elapsed() - m_lastFailure))));
    m_debounce.start(delay);
}

void SettingsSyncController::refresh()
{
    if (m_clock.elapsed() - m_lastAttempt >= 60000)
        schedule();
}

void SettingsSyncController::retry()
{
    if (m_persistenceFailed) {
        ++m_controlWrites;
        Async::runScoped(
            this, m_settings->retryPendingPersistence(),
            [this] {
                --m_controlWrites;
                m_persistenceFailed = false;
                m_problem.clear();
                persistControls(serializedLedger());
                schedule(0, true);
            },
            [this](const std::exception_ptr&) {
                --m_controlWrites;
                persistenceFailed();
            },
            "recover local settings transaction");
        return;
    }
    schedule(0, true);
}

void SettingsSyncController::setForeground(bool foreground)
{
    if (m_foreground == foreground)
        return;
    m_foreground = foreground;
    if (foreground) {
        if (m_enabled)
            m_refreshTimer.start();
        refresh();
    } else {
        invalidate();
        m_settings->cancelRemoteApplications({}, false);
        m_refreshTimer.stop();
    }
}

void SettingsSyncController::startCycle()
{
    if (m_busy || !m_loaded || !m_enabled || !m_foreground || !accountActive() || m_controlWrites
        || m_pendingLocalCommits || m_persistenceFailed)
        return;
    m_requested = false;
    m_manualRequested = false;
    m_busy = true;
    m_lastAttempt = m_clock.elapsed();
    m_problem.clear();
    m_nativeProblem.clear();
    m_storageProblem.clear();
    m_nativeOffline = false;
    m_storageOffline = false;
    const auto token = m_generation.current();
    emit changed();
    Async::runScoped(
        this, cycle(token), [this, token] { finishCycle(token); },
        [this, token](const std::exception_ptr& error) { finishCycle(token, exceptionMessage(error)); },
        "settings sync");
}

void SettingsSyncController::finishCycle(Token token, const QString& problem)
{
    m_busy = false;
    if (m_generation.isCurrent(token)) {
        if (!problem.isEmpty())
            m_problem = tr("Settings could not be synchronized. Retry. (%1)").arg(problem);
        if (!m_problem.isEmpty() || !m_nativeProblem.isEmpty() || !m_storageProblem.isEmpty())
            m_lastFailure = m_clock.elapsed();
    }
    emit changed();
    emit cycleFinished();
    if (m_requested)
        schedule(500);
}

void SettingsSyncController::setIntent(const QString& key, const QVariant& value, bool provisional)
{
    auto row = m_keys.value(key).toMap();
    QVariantMap intent { { QStringLiteral("value"), value },
        { QStringLiteral("generation"), row.value(QStringLiteral("generation"), QStringLiteral("0")) },
        { QStringLiteral("provisional"), provisional } };
    if (!provisional && backend(key) == QStringLiteral("spool")) {
        const auto entry = Doc::makeEntry(value, m_counter);
        m_counter = entry.clock;
        intent.insert(QStringLiteral("entry"), entryMap(entry));
        m_replica.insert(key, entry);
    }
    row.insert(QStringLiteral("intent"), intent);
    m_keys.insert(key, row);
    m_confirmed.remove(key);
}

QVariantMap SettingsSyncController::prepareLocalCommit(const QVariantMap& values)
{
    ++m_pendingLocalCommits;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const auto *spec = findSettingSpec(it.key());
        if (!spec || spec->syncPolicy == SettingSyncPolicy::Never)
            continue;
        auto row = m_keys.value(it.key()).toMap();
        row.insert(QStringLiteral("generation"), QString::number(keyGeneration(it.key()) + 1));
        m_keys.insert(it.key(), row);
        m_deferred.remove(it.key());
        if (!m_enabled || m_accountId.isEmpty() || !keyEnabled(it.key(), it.value())) {
            resetKey(it.key(), false);
            if (spec->syncPolicy == SettingSyncPolicy::PortableDefault
                && settingSyncPolicy(*spec, it.value()) == SettingSyncPolicy::DeviceOptIn) {
                auto paused = m_keys.value(it.key()).toMap();
                paused.insert(QStringLiteral("policyPaused"), true);
                m_keys.insert(it.key(), paused);
            }
            continue;
        }
        if (row.value(QStringLiteral("policyPaused")).toBool()) {
            // Returning from a device font is a policy re-enable, not a new
            // clocked preference that may defeat the requested remote baseline.
            resetKey(it.key(), false);
            continue;
        }
        const bool provisional
            = row.value(QStringLiteral("bootstrap"), true).toBool() || backend(it.key()) == QStringLiteral("none");
        setIntent(it.key(), it.value(), provisional);
    }
    return serializedLedger();
}

void SettingsSyncController::committed(const QVariantMap&)
{
    if (m_pendingLocalCommits > 0)
        --m_pendingLocalCommits;
    schedule(500);
    emit changed();
}

void SettingsSyncController::persistenceFailed()
{
    m_persistenceFailed = true;
    invalidate();
    m_problem = tr("Settings could not be saved locally. Sync is paused; Retry after storage is available.");
    emit changed();
}

bool SettingsSyncController::remoteApplicationAllowed(const QString& key, const QString& account) const
{
    return m_enabled && account == m_accountId && keyEnabled(key, m_settings->value(key));
}

void SettingsSyncController::beginEdit(const QString& key)
{
    m_editing.insert(key);
}

void SettingsSyncController::endEdit(const QString& key, bool changedValue)
{
    m_editing.remove(key);
    if (m_deferred.contains(key)) {
        const auto value = m_deferred.take(key);
        if (!changedValue && value.generation == keyGeneration(key)) {
            Async::runScoped(
                this, applyDeferred(key, value, m_generation.current()), [] {},
                [this](const std::exception_ptr&) { persistenceFailed(); }, "deferred settings sync");
        }
    }
    schedule(500);
}

QCoro::Task<void> SettingsSyncController::applyDeferred(QString key, Deferred value, Token token)
{
    if (!current(token) || value.generation != keyGeneration(key) || !keyEnabled(key, value.value)
        || !valueSupported(key, value.value))
        co_return;
    co_await m_settings->applyValues({ { key, value.value } }, ChangeOrigin::RemoteSync, serializedLedger());
}

QVariantMap SettingsSyncController::applications(const QVariantMap& candidates)
{
    QVariantMap apply;
    for (auto it = candidates.cbegin(); it != candidates.cend(); ++it) {
        if (!keyEnabled(it.key(), m_settings->value(it.key())) || !keyEnabled(it.key(), it.value())
            || !valueSupported(it.key(), it.value()))
            continue;
        const auto intent = m_keys.value(it.key()).toMap().value(QStringLiteral("intent")).toMap();
        if (!intent.isEmpty() && intent.value(QStringLiteral("value")) != it.value())
            continue;
        if (m_editing.contains(it.key())) {
            m_deferred.insert(it.key(), { it.value(), keyGeneration(it.key()) });
            continue;
        }
        apply.insert(it.key(), it.value());
    }
    return apply;
}

QCoro::Task<void> SettingsSyncController::persistReplica(QVariantMap candidates, Token token)
{
    if (!current(token))
        co_return;
    co_await m_settings->applyValues(applications(candidates), ChangeOrigin::RemoteSync, serializedLedger());
}

QCoro::Task<void> SettingsSyncController::cycle(Token token)
{
    QPointer<SettingsSyncController> guard(this);
    co_await m_settings->recoverUnfinishedApplications();
    if (!guard || !current(token))
        co_return;
    if (m_registry->hasCapability(m_accountId, Preferences)) {
        try {
            co_await nativeCycle(token);
        } catch (const std::exception& error) {
            if (!guard || !current(token))
                co_return;
            const QString code = QString::fromUtf8(error.what());
            m_nativeOffline = offlineProblem(code);
            if (code == QStringLiteral("preferences_unavailable") || code == QStringLiteral("permission_denied")) {
                // A known absent/read-denied native channel is not a writer.
                // The storage path performs its own remote-first bootstrap.
                m_nativeKnown = true;
                m_nativeWritable.clear();
            }
            m_nativeProblem = tr("Service preferences are unavailable. Retry. (%1)").arg(code);
        }
        if (!guard || !current(token))
            co_return;
    } else {
        m_nativeKnown = true;
        m_nativeWritable.clear();
    }
    if (m_storageAvailable) {
        try {
            co_await storageCycle(token);
        } catch (const std::exception& error) {
            if (!guard || !current(token))
                co_return;
            m_storageOffline = offlineProblem(QString::fromUtf8(error.what()));
            m_storageProblem
                = tr("Spool-specific sync is unavailable. Retry. (%1)").arg(QString::fromUtf8(error.what()));
        }
    }
}

QCoro::Task<void> SettingsSyncController::nativeCycle(Token token)
{
    QPointer<SettingsSyncController> guard(this);
    QMap<QString, quint64> readGenerations;
    for (const auto& spec : settingSpecs())
        if (spec.nativePreference[0])
            readGenerations.insert(QString::fromLatin1(spec.key), keyGeneration(QString::fromLatin1(spec.key)));
    const auto response = co_await m_registry->callSource(m_accountId, QStringLiteral("preferencesRead"), {}, Scope);
    if (!guard || !current(token))
        co_return;
    m_nativeKnown = true;
    m_nativeWritable.clear();
    for (const auto& value : response.value(QStringLiteral("writable")).toList())
        m_nativeWritable.insert(value.toString());
    const auto observed = response.value(QStringLiteral("values")).toMap();
    QVariantMap candidates;
    for (const auto& spec : settingSpecs()) {
        if (!spec.nativePreference[0])
            continue;
        const auto key = QString::fromLatin1(spec.key);
        const auto field = QString::fromLatin1(spec.nativePreference);
        if (backend(key) != QStringLiteral("native") || !keyEnabled(key, m_settings->value(key)))
            continue;
        auto row = m_keys.value(key).toMap();
        if (row.value(QStringLiteral("backend")).toString() != QStringLiteral("native")) {
            row.insert(QStringLiteral("backend"), QStringLiteral("native"));
            row.insert(QStringLiteral("bootstrap"), true);
            m_replica.remove(key);
        }
        const bool bootstrap = row.value(QStringLiteral("bootstrap"), true).toBool();
        row.insert(QStringLiteral("bootstrap"), false);
        m_keys.insert(key, row);
        auto intent = row.value(QStringLiteral("intent")).toMap();
        if (!intent.isEmpty()) {
            intent.insert(QStringLiteral("provisional"), false);
            intent.remove(QStringLiteral("entry"));
            row.insert(QStringLiteral("intent"), intent);
            m_keys.insert(key, row);
        } else if (observed.contains(field)) {
            if (readGenerations.value(key) == keyGeneration(key))
                candidates.insert(key, observed.value(field));
        } else if (bootstrap && valueSupported(key, m_settings->value(key))) {
            setIntent(key, m_settings->value(key), false);
        }
    }
    co_await persistReplica(candidates, token);
    if (!guard || !current(token))
        co_return;
    if (m_pendingLocalCommits || m_controlWrites) {
        m_requested = true;
        co_return;
    }

    QVariantMap writeValues;
    QMap<QString, quint64> captured;
    for (const auto& spec : settingSpecs()) {
        if (!spec.nativePreference[0])
            continue;
        const auto key = QString::fromLatin1(spec.key);
        if (backend(key) != QStringLiteral("native") || m_editing.contains(key)
            || !keyEnabled(key, m_settings->value(key)))
            continue;
        const auto intent = m_keys.value(key).toMap().value(QStringLiteral("intent")).toMap();
        if (intent.isEmpty()) {
            if (observed.contains(QString::fromLatin1(spec.nativePreference))
                && valueSupported(key, observed.value(QString::fromLatin1(spec.nativePreference))))
                m_confirmed.insert(key);
            continue;
        }
        writeValues.insert(QString::fromLatin1(spec.nativePreference), intent.value(QStringLiteral("value")));
        captured.insert(key, keyGeneration(key));
    }
    if (writeValues.isEmpty())
        co_return;
    co_await m_registry->callSource(
        m_accountId, QStringLiteral("preferencesWrite"), { { QStringLiteral("values"), writeValues } }, Scope);
    if (!guard || !current(token))
        co_return;
    const auto readback = co_await m_registry->callSource(m_accountId, QStringLiteral("preferencesRead"), {}, Scope);
    if (!guard || !current(token))
        co_return;
    const auto actual = readback.value(QStringLiteral("values")).toMap();
    bool differed = false;
    QSet<QString> acknowledged;
    for (auto it = captured.cbegin(); it != captured.cend(); ++it) {
        const auto *spec = findSettingSpec(it.key());
        const auto field = QString::fromLatin1(spec->nativePreference);
        if (!actual.contains(field) || actual.value(field) != writeValues.value(field)) {
            differed = true;
            continue;
        }
        if (it.value() != keyGeneration(it.key()))
            continue;
        auto row = m_keys.value(it.key()).toMap();
        row.remove(QStringLiteral("intent"));
        m_keys.insert(it.key(), row);
        acknowledged.insert(it.key());
    }
    // The exact generations are removed in the durable ledger before a green
    // state can appear. A commit during this await still has its own new intent.
    co_await persistReplica({}, token);
    if (!guard || !current(token))
        co_return;
    for (const auto& key : acknowledged)
        if (captured.value(key) == keyGeneration(key))
            m_confirmed.insert(key);
    if (differed)
        throw std::runtime_error("native_preference_readback_differed");
}

void SettingsSyncController::observeDocument(const Entries& observed, const QMap<QString, quint64>& readGenerations)
{
    m_counter = Doc::maximumClock(observed, m_counter);
    for (const auto& spec : settingSpecs()) {
        const auto key = QString::fromLatin1(spec.key);
        if (backend(key) != QStringLiteral("spool") || !keyEnabled(key, m_settings->value(key))) {
            m_replica.remove(key);
            continue;
        }
        auto row = m_keys.value(key).toMap();
        if (row.value(QStringLiteral("backend")).toString() != QStringLiteral("spool")) {
            row.insert(QStringLiteral("backend"), QStringLiteral("spool"));
            row.insert(QStringLiteral("bootstrap"), true);
        }
        const bool bootstrap = row.value(QStringLiteral("bootstrap"), true).toBool();
        if (bootstrap) {
            m_replica.remove(key);
            if (observed.contains(key))
                m_replica.insert(key, observed.value(key));
            row.insert(QStringLiteral("bootstrap"), false);
            m_keys.insert(key, row);
            const auto intent = row.value(QStringLiteral("intent")).toMap();
            if (!intent.isEmpty())
                setIntent(key, intent.value(QStringLiteral("value")), false);
            else if (!observed.contains(key) && valueSupported(key, m_settings->value(key)))
                setIntent(key, m_settings->value(key), false);
        } else {
            if (observed.contains(key)
                && (!m_replica.contains(key) || Doc::compareStamps(observed.value(key), m_replica.value(key)) > 0))
                m_replica.insert(key, observed.value(key));
            const auto intent = row.value(QStringLiteral("intent")).toMap();
            if (intent.value(QStringLiteral("provisional")).toBool())
                setIntent(key, intent.value(QStringLiteral("value")), false);
        }
        // An explicit remote winner may outrank a previously committed local
        // edit. It remains a retained maximum, not a new user commit/echo.
        auto latest = m_keys.value(key).toMap();
        const auto intent = latest.value(QStringLiteral("intent")).toMap();
        if (intent.contains(QStringLiteral("entry")) && m_replica.contains(key) && readGenerations.contains(key)
            && readGenerations.value(key) == keyGeneration(key)
            && Doc::compareStamps(m_replica.value(key), mapEntry(intent.value(QStringLiteral("entry")).toMap())) > 0) {
            latest.remove(QStringLiteral("intent"));
            m_keys.insert(key, latest);
        }
    }
}

SettingsSyncController::Entries SettingsSyncController::documentToWrite(const Entries& observed) const
{
    Entries result = observed;
    for (const auto& spec : settingSpecs()) {
        const auto key = QString::fromLatin1(spec.key);
        if (!retainDocumentKey(key) || backend(key) == QStringLiteral("native")) {
            result.remove(key);
            continue;
        }
        if (backend(key) != QStringLiteral("spool") || !keyEnabled(key, m_settings->value(key))
            || m_editing.contains(key))
            continue;
        const auto entry = m_replica.constFind(key);
        // A system font without explicit opt-in is carried only from this
        // latest server read; it must never be repaired from a retained copy.
        if (entry == m_replica.cend() || !keyEnabled(key, entry->value))
            continue;
        if (!result.contains(key) || Doc::compareStamps(*entry, result.value(key)) > 0)
            result.insert(key, *entry);
    }
    return result;
}

QCoro::Task<void> SettingsSyncController::storageCycle(Token token)
{
    QPointer<SettingsSyncController> guard(this);
    const auto info = co_await m_registry->callSource(m_accountId, QStringLiteral("dataInfo"), {}, Scope);
    if (!guard || !current(token))
        co_return;
    const auto maximum = std::min<qsizetype>(Doc::MaximumBytes, info.value(QStringLiteral("maxBytes")).toInt());
    if (maximum <= 0)
        throw std::runtime_error("invalid_storage_limit");
    const bool conditional = info.value(QStringLiteral("conditionalWrites")).toBool();
    QMap<QString, quint64> readGenerations;
    for (const auto& spec : settingSpecs())
        readGenerations.insert(QString::fromLatin1(spec.key), keyGeneration(QString::fromLatin1(spec.key)));
    auto response = co_await m_registry->callSource(
        m_accountId, QStringLiteral("dataRead"), { { QStringLiteral("key"), DocumentId } }, Scope);
    if (!guard || !current(token))
        co_return;
    const auto decode = [maximum](const QVariantMap& data) {
        if (!data.value(QStringLiteral("found")).toBool())
            return Entries {};
        const auto document = Doc::decode(data.value(QStringLiteral("value")), retainDocumentKey, maximum);
        if (document.status == Doc::DecodeStatus::UnsupportedFormat)
            throw std::runtime_error("Update Spool to read this settings document.");
        if (document.status != Doc::DecodeStatus::Valid)
            throw std::runtime_error("Malformed settings document; it has not been overwritten.");
        return document.entries;
    };
    auto observed = decode(response);
    for (int attempt = 0; attempt != 2; ++attempt) {
        observeDocument(observed, readGenerations);
        QVariantMap candidates;
        for (auto it = m_replica.cbegin(); it != m_replica.cend(); ++it)
            candidates.insert(it.key(), it->value);
        co_await persistReplica(candidates, token);
        if (!guard || !current(token))
            co_return;
        if (m_pendingLocalCommits || m_controlWrites) {
            m_requested = true;
            co_return;
        }
        const auto desired = documentToWrite(observed);
        const auto encoded = Doc::encode(desired, retainDocumentKey, maximum);
        const bool needsWrite = encoded != Doc::encode(observed, retainDocumentKey, maximum);
        QMap<QString, quint64> captured;
        for (auto it = desired.cbegin(); it != desired.cend(); ++it)
            if (backend(it.key()) == QStringLiteral("spool") && keyEnabled(it.key(), m_settings->value(it.key()))
                && !m_editing.contains(it.key()))
                captured.insert(it.key(), keyGeneration(it.key()));
        bool conflict = false;
        if (needsWrite) {
            QVariantMap args { { QStringLiteral("key"), DocumentId }, { QStringLiteral("value"), encoded } };
            if (conditional) {
                if (response.value(QStringLiteral("found")).toBool()) {
                    const auto revision = response.value(QStringLiteral("revision")).toString();
                    if (revision.isEmpty())
                        throw std::runtime_error("missing_storage_revision");
                    args.insert(QStringLiteral("expectedRevision"), revision);
                } else
                    args.insert(QStringLiteral("expectedRevision"), QVariant::fromValue(nullptr));
            }
            try {
                co_await m_registry->callSource(m_accountId, QStringLiteral("dataWrite"), args, Scope);
            } catch (const std::exception& error) {
                if (!conditional || !QString::fromUtf8(error.what()).contains(QStringLiteral("conflict")))
                    throw;
                conflict = true;
            }
            if (!guard || !current(token))
                co_return;
            response = co_await m_registry->callSource(
                m_accountId, QStringLiteral("dataRead"), { { QStringLiteral("key"), DocumentId } }, Scope);
            if (!guard || !current(token))
                co_return;
            observed = decode(response);
            readGenerations = captured;
        }
        // Always merge the readback before acknowledging. A lower or missing
        // weak-store readback is not permission to forget the local maximum.
        observeDocument(observed, readGenerations);
        QSet<QString> acknowledged;
        for (auto it = captured.cbegin(); it != captured.cend(); ++it) {
            const auto entry = desired.constFind(it.key());
            const auto actual = observed.constFind(it.key());
            if (conflict || entry == desired.cend() || actual == observed.cend()
                || Doc::compareStamps(*entry, *actual) != 0 || entry->value != actual->value
                || it.value() != keyGeneration(it.key()))
                continue;
            auto row = m_keys.value(it.key()).toMap();
            const auto intent = row.value(QStringLiteral("intent")).toMap();
            if (!intent.isEmpty()
                && (intent.value(QStringLiteral("value")) != entry->value || !intent.contains(QStringLiteral("entry"))
                    || Doc::compareStamps(mapEntry(intent.value(QStringLiteral("entry")).toMap()), *entry) != 0))
                continue;
            row.remove(QStringLiteral("intent"));
            m_keys.insert(it.key(), row);
            if (keyEnabled(it.key(), actual->value) && valueSupported(it.key(), actual->value))
                acknowledged.insert(it.key());
        }
        QVariantMap readbackCandidates;
        for (auto it = m_replica.cbegin(); it != m_replica.cend(); ++it)
            readbackCandidates.insert(it.key(), it->value);
        co_await persistReplica(readbackCandidates, token);
        if (!guard || !current(token))
            co_return;
        for (const auto& key : acknowledged)
            if (captured.value(key) == keyGeneration(key))
                m_confirmed.insert(key);
        const auto repaired = Doc::encode(documentToWrite(observed), retainDocumentKey, maximum);
        if (!conflict && repaired == Doc::encode(observed, retainDocumentKey, maximum))
            co_return;
        if (m_pendingLocalCommits) {
            m_requested = true;
            co_return;
        }
    }
    throw std::runtime_error("Settings changed concurrently. Pending changes are retained; Retry to converge.");
}

} // namespace Spool
