#pragma once

#include <QCoroTask>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

class QNetworkAccessManager;

namespace JellyfinNative {

class DatabaseManager;
class ProviderRegistry;

// Where providers come from and how they stay current.
//
// Two small JSON files on the store's GitHub Pages site describe everything:
// official.json lists first-party providers, index.json every provider in
// the store. Each entry names one release: its version, the archive URL and
// that archive's SHA-256, so a download is only installed when it is exactly
// what the store reviewed. A provider added by URL instead publishes the same
// entry itself, as a spool-provider.json release asset or any static file,
// and is kept current from there.
class ProviderStore final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList official READ official NOTIFY catalogChanged)
    Q_PROPERTY(QVariantList community READ community NOTIFY catalogChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY catalogChanged)
    Q_PROPERTY(QString error READ error NOTIFY catalogChanged)
    Q_PROPERTY(QVariantList updates READ updates NOTIFY updatesChanged)
    Q_PROPERTY(QVariantMap busy READ busy NOTIFY busyChanged)

public:
    static constexpr auto kApi = "0.2";

    ProviderStore(ProviderRegistry *registry, DatabaseManager *database, QNetworkAccessManager *network,
        QUrl catalogBase, QObject *parent = nullptr);

    QVariantList official() const;
    QVariantList community() const;
    bool loading() const
    {
        return m_loading > 0;
    }
    QString error() const
    {
        return m_error;
    }
    QVariantList updates() const
    {
        return m_updates;
    }
    // Provider id to what is happening to it: "downloading", "installing".
    QVariantMap busy() const
    {
        return m_busy;
    }

    // official.json always; index.json too when asked for or when something
    // from it is installed.
    Q_INVOKABLE void refresh(bool includeCommunity = true);
    Q_INVOKABLE void install(const QString& id);
    // A GitHub or GitLab project URL, or a direct link to a provider's
    // spool-provider.json anywhere else.
    Q_INVOKABLE void addFromUrl(const QString& url);
    Q_INVOKABLE void update(const QString& id);
    Q_INVOKABLE void updateAll();
    Q_INVOKABLE void uninstall(const QString& id);
    // At launch: check every installed provider against where it came from,
    // then install, prompt or wait according to the updates preference.
    void checkForUpdates(const QString& policy);

    static QUrl feedUrlFor(const QString& input);

signals:
    void catalogChanged();
    void updatesChanged();
    void busyChanged();
    void installed(const QString& id, const QString& name);
    void problem(const QString& message);

private:
    struct Origin {
        QString channel; // official, community or url
        QUrl feed;
    };

    QCoro::Task<QByteArray> fetch(QUrl url, qint64 limit);
    QCoro::Task<QVariantList> fetchCatalog(QString name);
    QCoro::Task<void> installEntry(QVariantMap entry, Origin origin);
    QCoro::Task<void> checkAsync(QString policy);
    QVariantList annotate(const QVariantList& entries) const;
    QVariantMap entryFor(const QString& id) const;
    void setBusy(const QString& id, const QString& state);
    void saveOrigins();

    QPointer<ProviderRegistry> m_registry;
    QPointer<DatabaseManager> m_database;
    QNetworkAccessManager *m_network;
    QUrl m_catalogBase;
    QVariantList m_official;
    QVariantList m_community;
    QVariantList m_updates;
    QVariantMap m_busy;
    QHash<QString, Origin> m_origins;
    QString m_error;
    int m_loading = 0;
    bool m_originsLoaded = false;
};

} // namespace JellyfinNative
