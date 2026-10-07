#pragma once

#include "../common/RequestGeneration.h"
#include "SettingsSyncDocument.h"

#include <QCoroTask>
#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

namespace Spool {
class SettingsController;
class DatabaseManager;
class ProviderRegistry;
struct SettingSpec;

// One selected account owns both channels. The local ledger is committed in
// the same database transaction as user values; network work never owns values.
class SettingsSyncController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(QString accountId READ accountId NOTIFY changed)
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY changed)
    Q_PROPERTY(QVariantMap states READ states NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString problemDetail READ problemDetail NOTIFY changed)
    Q_PROPERTY(bool customized READ customized NOTIFY changed)
    Q_PROPERTY(bool accountChangePending READ accountChangePending NOTIFY changed)
    Q_PROPERTY(QString pendingAccountId READ pendingAccountId NOTIFY changed)
    Q_PROPERTY(QString accountChangeWarning READ accountChangeWarning NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
public:
    SettingsSyncController(SettingsController *, DatabaseManager *, ProviderRegistry *, QObject * = nullptr);
    ~SettingsSyncController() override;
    QCoro::Task<void> loadLocalAsync();
    bool enabled() const
    {
        return m_enabled;
    }
    QString accountId() const
    {
        return m_accountId;
    }
    QVariantList accounts() const;
    QVariantMap states() const;
    QString summary() const;
    QString status() const;
    QString problemDetail() const;
    bool customized() const;
    bool accountChangePending() const
    {
        return !m_pendingAccountId.isEmpty();
    }
    QString pendingAccountId() const
    {
        return m_pendingAccountId;
    }
    QString accountChangeWarning() const;
    bool busy() const
    {
        return m_busy || m_controlWrites != 0;
    }

    Q_INVOKABLE void setEnabled(bool enabled);
    Q_INVOKABLE void setAccountId(const QString& accountId);
    Q_INVOKABLE void confirmAccountChange(bool approved);
    Q_INVOKABLE void setSettingEnabled(const QString& key, bool enabled);
    Q_INVOKABLE void setSettingsEnabled(const QStringList& keys, bool enabled);
    Q_INVOKABLE void resetSettingOverrides();
    Q_INVOKABLE void retry();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setForeground(bool foreground);
    Q_INVOKABLE void beginEdit(const QString& key);
    Q_INVOKABLE void endEdit(const QString& key, bool changed);

    // No I/O: SettingsController includes these serialized rows in its commit.
    QVariantMap prepareLocalCommit(const QVariantMap& normalizedValues);
    bool remoteApplicationAllowed(const QString& key, const QString& account) const;
    bool remoteApplicationReady() const
    {
        return m_loaded && m_enabled && m_foreground && accountActive();
    }

signals:
    void changed();
    void cycleFinished();

private:
    using Entries = SettingsSyncDocument::Entries;
    using Token = RequestGeneration::Token;
    struct Deferred {
        QVariant value;
        quint64 generation = 0;
    };
    bool current(Token token) const;
    bool accountActive() const;
    bool keyEnabled(const QString& key, const QVariant& value) const;
    bool valueSupported(const QString& key, const QVariant& value) const;
    static bool retainDocumentKey(const QString& key);
    QString backend(const QString& key) const;
    quint64 keyGeneration(const QString& key) const;
    QVariantMap serializedLedger() const;
    QVariantMap serializedControls() const;
    void restoreLedger(const QString& serialized);
    void resetKey(const QString& key, bool cancelApplications = true);
    void invalidate();
    void reconcileAccounts();
    void selectAccount(const QString& accountId);
    void persistControls(QVariantMap extra = {});
    void schedule(int delay = 0, bool manual = false);
    void startCycle();
    void finishCycle(Token token, const QString& problem = {});
    void committed(const QVariantMap& values);
    void persistenceFailed();
    void setIntent(const QString& key, const QVariant& value, bool provisional);
    void observeDocument(const Entries& observed, const QMap<QString, quint64>& readGenerations);
    Entries documentToWrite(const Entries& observed) const;
    QVariantMap applications(const QVariantMap& candidates);
    QCoro::Task<void> persistReplica(QVariantMap candidates, Token token);
    QCoro::Task<void> cycle(Token token);
    QCoro::Task<void> nativeCycle(Token token);
    QCoro::Task<void> storageCycle(Token token);
    QCoro::Task<void> applyDeferred(QString key, Deferred value, Token token);

    SettingsController *m_settings;
    DatabaseManager *m_database;
    ProviderRegistry *m_registry;
    RequestGeneration m_generation;
    QTimer m_debounce;
    QTimer m_refreshTimer;
    QElapsedTimer m_clock;
    qint64 m_lastAttempt = -60000;
    qint64 m_lastFailure = -30000;
    bool m_enabled = true;
    bool m_loaded = false;
    bool m_foreground = true;
    bool m_busy = false;
    bool m_requested = false;
    bool m_manualRequested = false;
    bool m_persistenceFailed = false;
    int m_controlWrites = 0;
    int m_pendingLocalCommits = 0;
    QString m_accountId;
    QString m_pendingAccountId;
    QString m_problem;
    QString m_nativeProblem;
    QString m_storageProblem;
    QVariantMap m_overrides;
    QVariantMap m_keys;
    Entries m_replica;
    QString m_counter = QStringLiteral("0");
    QSet<QString> m_nativeWritable;
    bool m_nativeKnown = false;
    bool m_storageAvailable = false;
    bool m_nativeOffline = false;
    bool m_storageOffline = false;
    QSet<QString> m_editing;
    QMap<QString, Deferred> m_deferred;
    QSet<QString> m_confirmed;
    QString m_lastConnectionState;
};
} // namespace Spool
