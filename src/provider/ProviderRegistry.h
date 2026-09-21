#pragma once

#include "Provider.h"
#include "ProviderMediaPage.h"
#include <QCoroTask>
#include <QUrl>
#include <QVariantList>
#include <memory>

#include <QObject>
#include <QString>

#include <vector>

namespace JellyfinNative {

class DatabaseManager;

// The active provider's capability flags as twelve booleans, registered in
// QML as the ProviderCapabilities singleton. The property names are the
// contract with shared QML: a gated control binds to one of them by name.
class ProviderCapabilities final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool auth READ auth NOTIFY changed)
    Q_PROPERTY(bool discovery READ discovery NOTIFY changed)
    Q_PROPERTY(bool search READ search NOTIFY changed)
    Q_PROPERTY(bool userItemState READ userItemState NOTIFY changed)
    Q_PROPERTY(bool playbackReporting READ playbackReporting NOTIFY changed)
    Q_PROPERTY(bool segments READ segments NOTIFY changed)
    Q_PROPERTY(bool libraryManagement READ libraryManagement NOTIFY changed)
    Q_PROPERTY(bool syncPlay READ syncPlay NOTIFY changed)
    Q_PROPERTY(bool remoteControl READ remoteControl NOTIFY changed)
    Q_PROPERTY(bool quickConnect READ quickConnect NOTIFY changed)
    Q_PROPERTY(bool peerRelay READ peerRelay NOTIFY changed)
    Q_PROPERTY(bool streamQuality READ streamQuality NOTIFY changed)

public:
    explicit ProviderCapabilities(QObject *parent = nullptr);

    Provider::Capabilities flags() const
    {
        return m_flags;
    }
    void setFlags(Provider::Capabilities flags);

    bool auth() const
    {
        return m_flags.testFlag(Provider::Auth);
    }
    bool discovery() const
    {
        return m_flags.testFlag(Provider::Discovery);
    }
    bool search() const
    {
        return m_flags.testFlag(Provider::Search);
    }
    bool userItemState() const
    {
        return m_flags.testFlag(Provider::UserItemState);
    }
    bool playbackReporting() const
    {
        return m_flags.testFlag(Provider::PlaybackReporting);
    }
    bool segments() const
    {
        return m_flags.testFlag(Provider::Segments);
    }
    bool libraryManagement() const
    {
        return m_flags.testFlag(Provider::LibraryManagement);
    }
    bool syncPlay() const
    {
        return m_flags.testFlag(Provider::SyncPlay);
    }
    bool remoteControl() const
    {
        return m_flags.testFlag(Provider::RemoteControl);
    }
    bool quickConnect() const
    {
        return m_flags.testFlag(Provider::QuickConnect);
    }
    bool peerRelay() const
    {
        return m_flags.testFlag(Provider::PeerRelay);
    }
    bool streamQuality() const
    {
        return m_flags.testFlag(Provider::StreamQuality);
    }

signals:
    void changed();

private:
    Provider::Capabilities m_flags;
};

// Every provider the build knows about, and the one the app is running on.
// Providers are added by whoever constructs them and are not owned here.
class ProviderRegistry final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList configuredSources READ configuredSources NOTIFY configuredSourcesChanged)

public:
    explicit ProviderRegistry(QObject *parent = nullptr);
    ~ProviderRegistry() override;

    // Portable modules are process-wide; configured source factories are
    // independent accounts/servers. Browsing selection never owns their life.
    void registerModule(const QString& moduleId, const QString& entryPoint);
    QCoro::Task<void> restoreSources(DatabaseManager *database);
    QCoro::Task<QString> configureSource(QString moduleId, QString accountId, QString sourceKey, QString label,
        QVariantMap configuration, QList<QUrl> authorisedOrigins);
    QCoro::Task<QVariantMap> callSource(
        QString sourceId, QString operation, QVariantMap arguments = {}, QString scope = {});
    QCoro::Task<ProviderMediaPage> callSourceMediaPage(QString sourceId, QString operation,
        QVariantMap arguments = {}, QString scope = {}, int maximumItems = 100);
    void cancelSourceScope(const QString& sourceId, const QString& scope);
    QCoro::Task<void> setSourceEnabled(QString sourceId, bool enabled);
    QCoro::Task<void> removeSource(QString sourceId);
    QVariantList configuredSources() const;

    void add(Provider *provider);
    Provider *provider(const QString& id) const;
    const std::vector<Provider *>& providers() const
    {
        return m_providers;
    }

    // Returns false when no provider carries that id; the active one is
    // then unchanged.
    bool setActive(const QString& id);
    void setActive(Provider *provider);
    Provider *active() const
    {
        return m_active;
    }
    ProviderCapabilities *capabilities()
    {
        return &m_capabilities;
    }

signals:
    void activeChanged();
    void configuredSourcesChanged();
    void sourceRemoved(const QString& sourceId);

private:
    void refreshCapabilities();
    void refreshSourceSnapshot();
    QCoro::Task<void> persistSources();
    struct PortableState;
    std::unique_ptr<PortableState> m_portable;

    std::vector<Provider *> m_providers;
    Provider *m_active = nullptr;
    QMetaObject::Connection m_activeCapabilities;
    ProviderCapabilities m_capabilities;
};

} // namespace JellyfinNative
