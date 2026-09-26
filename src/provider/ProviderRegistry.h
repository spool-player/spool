#pragma once

#include "ProviderMediaPage.h"
#include "ProviderPackage.h"
#include "ScriptRuntime.h"

#include <QCoroTask>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QThreadPool>
#include <QUrl>
#include <QVariantList>

#include <functional>
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
    void addNativeModule(ProviderManifest manifest, ProviderModule::NativeFactory factory);
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
    void cancelSourceScope(const QString& sourceId, const QString& scope);
    bool sourceRunning(const QString& sourceId) const;

    QVariantList modules() const;
    QVariantList accounts() const;

    // Mounting provider QML. beginSetup returns a context for the module's
    // login component, or null after adding an account straight away for a
    // provider that needs no sign-in.
    Q_INVOKABLE QObject *beginSetup(const QString& moduleId);
    Q_INVOKABLE QObject *openSettings(const QString& accountId);
    Q_INVOKABLE QObject *openPicker(const QString& accountId, const QVariantMap& arguments);
    // Shows the account's picker component and waits for the viewer's
    // choice; empty when they back out.
    QCoro::Task<QVariantMap> pick(QString accountId, QVariantMap arguments);
    Q_INVOKABLE QUrl componentUrl(const QString& moduleId, const QString& role) const;
    Q_INVOKABLE void useAccount(const QString& accountId);
    // Starts the accounts set aside for another user of the same server, so
    // search can reach libraries the account in use cannot. They stay out of
    // browsing; SourceHub decides which of them a search needs.
    void startSetAside();
    Q_INVOKABLE void setAccountEnabled(const QString& accountId, bool enabled);
    Q_INVOKABLE void removeAccount(const QString& accountId);

    // Called by ProviderUiContext.
    QCoro::Task<void> allowSetupOrigin(QString draftId, QUrl origin);
    QString finishSetup(const QString& draftId, const QVariantMap& result);
    void updateConfiguration(const QString& accountId, const QVariantMap& changes);
    void restartAccount(const QString& accountId);
    void endContext(const QString& sourceId);

signals:
    void modulesChanged();
    void accountsChanged();
    void restoredChanged();
    void sourceStarted(Spool::Provider *provider);
    void sourceStopped(const QString& accountId);
    void accountAdded(const QString& accountId);
    void problem(const QString& message);
    // A provider component the shell should mount now (a picker).
    void componentRequested(QObject *context);

private:
    struct Running {
        QString module;
        quint64 generation = 0;
        QPointer<Provider> provider;
        QList<QUrl> origins;
        bool draft = false;
    };

    ProviderAccount *account(const QString& id);
    ScriptRuntime *runtimeFor(ProviderModule& module);
    QCoro::Task<void> start(QString accountId);
    void stop(const QString& accountId);
    void restartModule(const QString& moduleId);
    // Account metadata goes to the database; configuration, which holds
    // credentials, goes to the platform credential store when it changed.
    void persist(bool credentials = false);
    void registerPackage(ProviderManifest manifest, QUrl root, bool bundled);
    void handleEvent(const QString& sourceId, const QString& type, const QVariantMap& payload);
    void handleInterrupted(const QString& moduleId);
    ProviderUiContext *createContext(const QString& sourceId, const QString& role, const QString& moduleId);
    template <typename T, typename Call> QCoro::Task<T> guarded(QString sourceId, Call call);

    QPointer<DatabaseManager> m_database;
    QString m_installDirectory;
    QVariantMap m_device;
    ScriptRuntime::NetworkHooks m_hooks;
    QHash<QString, ProviderModule> m_modules;
    std::vector<ProviderAccount> m_accounts;
    QHash<QString, Running> m_running;
    quint64 m_nextGeneration = 0;
    bool m_restored = false;
    QStringList m_removedAccounts;
    QSet<QString> m_expired;
    QThreadPool m_credentialPool;
};

} // namespace Spool
