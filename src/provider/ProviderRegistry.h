#pragma once

#include "Provider.h"

#include <QObject>
#include <QString>

#include <vector>

namespace JellyfinNative {

// The active provider's capability flags as eleven booleans, registered in
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

signals:
    void changed();

private:
    Provider::Capabilities m_flags;
};

// Every provider the build knows about, and the one the app is running on.
// Providers are added by whoever constructs them and are not owned here.
class ProviderRegistry final : public QObject {
    Q_OBJECT

public:
    explicit ProviderRegistry(QObject *parent = nullptr);

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

private:
    void refreshCapabilities();

    std::vector<Provider *> m_providers;
    Provider *m_active = nullptr;
    QMetaObject::Connection m_activeCapabilities;
    ProviderCapabilities m_capabilities;
};

} // namespace JellyfinNative
