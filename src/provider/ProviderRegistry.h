#pragma once

#include "ProviderExtensionData.h"
#include "ProviderMediaPage.h"
#include "ProviderPackage.h"
#include "ScriptRuntime.h"

#include <QCoroTask>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QPromise>
#include <QSet>
#include <QThreadPool>
#include <QUrl>
#include <QVariantList>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace Spool {

class DatabaseManager;
class PortableProvider;
class Provider;
class ProviderUiContext;

struct ProviderModule {
    ProviderManifest manifest;
    // qrc:/providers/<id>/ for bundled packages, a version directory for
    // installed ones; empty for native modules.
    QUrl root;
    bool bundled = false;
    // A newer installed package shadows the bundled one until removed.
    bool overridesBundled = false;
    using NativeFactory
        = std::function<Provider *(const QString& accountId, const QVariantMap& configuration, QObject *parent)>;
    NativeFactory native;
    ScriptRuntime *runtime = nullptr;
    bool failed = false;

    QUrl file(const QString& relative) const
    {
        return relative.isEmpty() || root.isEmpty() ? QUrl() : root.resolved(QUrl(relative));
    }
};

// One sign-in on one provider. `group` names accounts that are alternatives
// to one another, such as users of one server: using one sets the others
// aside, while accounts in different groups are shown together.
struct ProviderAccount {
    QString id;
    QString module;
    QString key;
    QString group;
    QString label;
    QString detail;
    bool enabled = true;
    QVariantMap configuration;
    QList<QUrl> origins;
    qint64 lastUsed = 0;
    // Provider-defined identity, scoped to this module; never authentication proof.
    QString activationFamily;
    QString activationIdentity;
};

// Every provider the app can run and every account signed in to one.
// Accounts persist with their configuration (credentials included, as the
// old per-server profiles did) and each enabled one runs as a Provider that
// SourceHub routes to.
class ProviderRegistry final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList modules READ modules NOTIFY modulesChanged)
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)
    Q_PROPERTY(bool restored READ restored NOTIFY restoredChanged)
    Q_PROPERTY(bool hasAccounts READ hasAccounts NOTIFY accountsChanged)
    Q_PROPERTY(QVariantMap networkConsent READ networkConsent NOTIFY networkConsentChanged)
    // Some server's viewers wait for an explicit choice at startup.
    Q_PROPERTY(bool startupChoicePending READ startupChoicePending NOTIFY accountsChanged)

public:
    explicit ProviderRegistry(DatabaseManager *database, QObject *parent = nullptr);
    ~ProviderRegistry() override;

    void setRuntimeEnvironment(QVariantMap device, ScriptRuntime::NetworkHooks hooks);
    // Left unset, nothing is installed or loaded from disk.
    void setInstallDirectory(const QString& path);
    QString installDirectory() const
    {
        return m_installDirectory;
    }
    // Registers every package bundled at qrc:/providers/ and every package
    // installed on disk; the newer version of a provider wins.
    void loadModules();
    void addNativeModule(ProviderManifest manifest, ProviderModule::NativeFactory factory, QUrl uiRoot = {});
    const ProviderModule *module(const QString& id) const;
    QStringList moduleIds() const;

    // Reads the saved accounts and starts the enabled ones in parallel.
    QCoro::Task<void> restore();
    bool restored() const
    {
        return m_restored;
    }
    bool hasAccounts() const
    {
        return !m_accounts.empty();
    }
    const std::vector<ProviderAccount>& accountList() const
    {
        return m_accounts;
    }

    // Replaces a module's code in place: running accounts restart on the new
    // version without the app restarting.
    QCoro::Task<void> install(ProviderPackageContents package);
    QCoro::Task<void> uninstall(QString moduleId);

    QCoro::Task<QVariantMap> callSource(
        QString sourceId, QString operation, QVariantMap arguments = {}, QString scope = {});
    QCoro::Task<ProviderMediaPage> callSourceMediaPage(
        QString sourceId, QString operation, QVariantMap arguments = {}, int maximumItems = 100, QString scope = {});
    QCoro::Task<MovieItem> callSourceItem(QString sourceId, QString operation, QVariantMap arguments = {});
    bool hasCapability(const QString& accountId, const QString& capability) const;
    QVariantMap capabilities(const QString& sourceId) const;
    void cancelSourceScope(const QString& sourceId, const QString& scope);
    bool sourceRunning(const QString& sourceId) const;
    bool accountOriginAllowed(const QString& accountId, const QUrl& url) const;
    QString deviceId() const
    {
        return m_device.value(QStringLiteral("id")).toString();
    }
    QVariantMap activationConfiguration(const QString& accountId) const;

    QVariantList modules() const;
    QVariantList accounts() const;

    // Mounting provider QML. beginSetup returns a context for the module's
    // login component, or null after adding an account straight away for a
    // provider that needs no sign-in.
    Q_INVOKABLE QObject *beginSetup(
        const QString& moduleId, const QString& accountId = {}, const QString& purpose = QStringLiteral("addProfile"));
    Q_INVOKABLE QObject *openSettings(const QString& accountId);
    Q_INVOKABLE QObject *openPicker(const QString& accountId, const QVariantMap& arguments);
    // Shows the account's picker component and waits for the viewer's
    // choice; empty when they back out.
    QCoro::Task<QVariantMap> pick(QString accountId, QVariantMap arguments, QString scope = {});
    Q_INVOKABLE QUrl componentUrl(const QString& moduleId, const QString& role) const;
    Q_INVOKABLE void useAccount(const QString& accountId);
    Q_INVOKABLE void setAccountEnabled(const QString& accountId, bool enabled);
    Q_INVOKABLE void removeAccount(const QString& accountId);
    // Abandons a pending activation; the current viewer stays as it was.
    Q_INVOKABLE void cancelActivation(const QString& accountId);
    // What a profile set does at startup: "always" opens this account's
    // (authorized, active) profile; "ask" waits for an explicit choice.
    Q_INVOKABLE bool setStartupChoice(const QString& accountId, const QString& mode);
    // The new-account startup question was answered elsewhere or dismissed.
    Q_INVOKABLE void finishOnboarding(const QString& accountId);
    bool startupChoicePending() const
    {
        return !m_awaitingChoice.isEmpty();
    }

    // Called by ProviderUiContext.
    QCoro::Task<void> allowSetupOrigin(QString draftId, QUrl origin);
    QCoro::Task<void> requestAccountOrigin(QString accountId, QUrl origin, QString scope = {});
    QCoro::Task<void> allowLanDiscovery(QString draftId, QString scope);
    void cancelLanDiscovery(const QString& draftId, const QString& scope);
    QVariantMap networkConsent() const
    {
        return m_networkConsent;
    }
    Q_INVOKABLE void resolveNetworkConsent(const QString& requestId, bool approved);
    QString finishSetup(const QString& draftId, const QVariantMap& result);
    void updateConfiguration(const QString& accountId, const QVariantMap& changes);
    void restartAccount(const QString& accountId, const QVariantMap& changes = {});
    void endContext(const QString& sourceId);

signals:
    void modulesChanged();
    void accountsChanged();
    void restoredChanged();
    void capabilitiesChanged(const QString& accountId);
    void networkConsentChanged();
    void sourceStarted(Spool::Provider *provider);
    void sourceStopped(const QString& accountId);
    void sourceScopeCancelled(const QString& sourceId, const QString& scope);
    void accountIdentityRevoked(const QString& accountId);
    void activationConfigurationChanged(const QString& accountId);
    void accountAdded(const QString& accountId);
    // Selection has committed or settled without changing the viewer.
    void accountSelectionFinished(const QString& accountId, bool selected);
    void problem(const QString& message);
    // A provider component the shell should mount now (a picker).
    void componentRequested(QObject *context);
    // Includes private prepared sources, which are never announced to SourceHub.
    void contextSourceStopped(const QString& sourceId);

private:
    struct CapabilityCall {
        QString capability;
        QString scope;
    };
    struct Running {
        QString module;
        QString runtimeId;
        QString accountId;
        quint64 generation = 0;
        QPointer<Provider> provider;
        QList<QUrl> origins;
        bool draft = false;
        QVariantMap setupConfiguration;
        bool enableOnCommit = false;
        QVariantMap declaredCapabilities;
        QVariantMap capabilities;
        QHash<QString, quint64> capabilityRevisions;
        QHash<quint64, CapabilityCall> capabilityCalls;
        std::optional<ProviderExtensionData::StorageInfo> storageInfo;
        bool lanConsent = false;
        quint64 networkRevision = 0;
        QVariantMap pendingConfiguration;
        QVariantMap pendingOptions;
        QList<QPair<QString, QVariantMap>> pendingEvents;
        std::shared_ptr<std::atomic_bool> activationApproval;
    };

    ProviderAccount *account(const QString& id);
    ScriptRuntime *runtimeFor(ProviderModule& module);
    QCoro::Task<void> start(QString accountId, QString reason = QStringLiteral("startup"), bool select = false,
        std::optional<ProviderAccount> replacement = {});
    void stop(const QString& sourceId, bool revokeIdentity = false);
    void stopPublished(const QString& accountId);
    QString runtimeSourceId(const QString& sourceId) const;
    bool lastUsedIdentity(const ProviderAccount& candidate) const;
    void commitSelection(const ProviderAccount& candidate);
    struct PickerResult {
        bool submitted = false;
        QVariantMap values;
    };
    QCoro::Task<PickerResult> pickResult(
        QString sourceId, QVariantMap arguments, bool activation = false, QString scope = {});
    struct ActivationGrant {
        QString identity;
        QPointer<ScriptRuntime> runtime;
        quint64 generation = 0;
        QVariant value;
    };
    QString familyKey(const ProviderAccount& candidate) const;
    // Viewers that are alternatives to one another: one activation family, or
    // else one provider group (the users of one server).
    QString profileSet(const ProviderAccount& candidate) const;
    bool sameProfile(const ProviderAccount& a, const ProviderAccount& b) const;
    bool startupDefault(const ProviderAccount& candidate) const;
    void applyStartupChoices();
    void clearGrants(const QString& moduleId, const QString& family = {});
    QCoro::Task<void> startRestored(QStringList ids);
    void restartModule(const QString& moduleId);
    // Account metadata goes to the database; configuration, which holds
    // credentials, goes to the platform credential store when it changed.
    void persist(bool credentials = false);
    void registerPackage(ProviderManifest manifest, QUrl root, bool bundled);
    void handleEvent(const QString& sourceId, const QString& type, const QVariantMap& payload);
    void handleInterrupted(const QString& moduleId);
    ProviderUiContext *createContext(const QString& sourceId, const QString& role, const QString& moduleId);
    template <typename T, typename Call>
    QCoro::Task<T> guarded(QString sourceId, Call call, QString capability = {}, QString scope = {});
    bool sourceHasCapability(const QString& sourceId, const QString& capability) const;
    void updateCapabilities(const QString& sourceId, QVariantMap offers);
    QCoro::Task<bool> requestNetworkConsent(QString sourceId, QString scope, QString kind, QUrl origin = {});
    void cancelNetworkConsent(const QString& sourceId, const QString& scope = {});

    QPointer<DatabaseManager> m_database;
    QString m_installDirectory;
    QVariantMap m_device;
    ScriptRuntime::NetworkHooks m_hooks;
    QHash<QString, ProviderModule> m_modules;
    std::vector<ProviderAccount> m_accounts;
    QHash<QString, Running> m_running;
    QHash<QString, QString> m_runtimeSources;
    QHash<QString, QString> m_preparing;
    QHash<QString, ActivationGrant> m_activationGrants;
    QHash<QString, quint64> m_familyEpochs;
    QVariantMap m_activationOptions;
    // Device-local, by profile set: { mode: "always" | "ask", account }.
    QVariantMap m_startupChoices;
    QSet<QString> m_awaitingChoice;
    QSet<QString> m_onboarding;
    QSet<QString> m_lockedAccounts;
    quint64 m_nextGeneration = 0;
    quint64 m_nextCapabilityCall = 0;
    QVariantMap m_networkConsent;
    QSet<QString> m_removingAccounts;
    std::shared_ptr<QPromise<bool>> m_consentPromise;
    QString m_consentSource;
    QString m_consentScope;
    QSet<QString> m_failedAccounts;
    QHash<QString, QString> m_accountErrors;
    // Scripted accounts whose stored configuration is not a valid JSON object,
    // rather than accounts whose valid configuration happens to be empty.
    QSet<QString> m_unavailableConfigurations;
    bool m_restored = false;
    QStringList m_removedAccounts;
    QSet<QString> m_expired;
    QThreadPool m_credentialPool;
};

} // namespace Spool
