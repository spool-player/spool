#pragma once

#include <QByteArray>
#include <QCoroTask>
#include <QFuture>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVariant>

#include "../media/MediaTypes.h"

#include <optional>
#include <utility>
#include <vector>

namespace Spool {

class DatabaseWorker;

struct StartupState {
    QString deviceId;
    QVariantMap values;
};

class DatabaseManager final : public QObject {
    Q_OBJECT

public:
    explicit DatabaseManager(QObject *parent = nullptr);
    ~DatabaseManager() override;

    bool initialize(const QString& databasePath);
    void shutdown();

    // Sign-ins saved by the native Jellyfin client, for the one-time move to
    // provider accounts.
    QCoro::Task<QVariantList> loadLegacyAccountsAsync();
    QCoro::Task<QString> loadDeviceIdAsync();
    void saveDeviceId(const QString& deviceId);

    QCoro::Task<QJsonObject> loadHomePayloadAsync(const QString& key, int schemaVersion);
    void saveHomePayload(const QString& key, int schemaVersion, const QJsonObject& payload);
    void invalidateHomePayloads();

    QCoro::Task<QString> loadSettingAsync(const QString& key, const QString& defaultValue = {});
    QCoro::Task<QVariantMap> loadValuesAsync(const QStringList& keys);
    QCoro::Task<StartupState> loadStartupStateAsync(const QStringList& keys);
    void saveSetting(const QString& key, const QString& value);

    QCoro::Task<int> schemaVersionAsync();
    QCoro::Task<QByteArray> loadCacheEntryAsync(const QString& nameSpace, const QString& key, qint64 maxAgeMs = -1);
    void saveCacheEntry(const QString& nameSpace, const QString& key, const QByteArray& value, qint64 ttlMs = 0);
    void invalidateCacheNamespace(const QString& nameSpace);
    void evictCacheEntries(int maximumEntries);

signals:
    void initializationFailed(QString message);
    void recoveryNotice(QString message);

private:
    QCoro::Task<bool> awaitInitialization();
    template <typename Callback> void invokeOnWorkerAsync(Callback&& callback)
    {
        QMetaObject::invokeMethod(m_worker, std::forward<Callback>(callback), Qt::QueuedConnection);
    }

    QThread m_thread;
    DatabaseWorker *m_worker = nullptr;
    QFuture<bool> m_initializationFuture;
};

} // namespace Spool
