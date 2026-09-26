#pragma once

#include "Provider.h"

namespace Spool {

// What the enabled accounts can do between them, as the ProviderCapabilities
// QML singleton. Shared QML binds each optional control to one of these.
class ProviderCapabilities final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool search READ search NOTIFY changed)
    Q_PROPERTY(bool userItemState READ userItemState NOTIFY changed)
    Q_PROPERTY(bool playbackReporting READ playbackReporting NOTIFY changed)
    Q_PROPERTY(bool segments READ segments NOTIFY changed)
    Q_PROPERTY(bool groupPlayback READ groupPlayback NOTIFY changed)
    Q_PROPERTY(bool remoteControl READ remoteControl NOTIFY changed)
    Q_PROPERTY(bool streamQuality READ streamQuality NOTIFY changed)
    Q_PROPERTY(bool trickplay READ trickplay NOTIFY changed)
    Q_PROPERTY(bool speedTest READ speedTest NOTIFY changed)

public:
    using QObject::QObject;

    Provider::Capabilities flags() const
    {
        return m_flags;
    }
    void setFlags(Provider::Capabilities flags)
    {
        if (m_flags != flags) {
            m_flags = flags;
            emit changed();
        }
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
    bool groupPlayback() const
    {
        return m_flags.testFlag(Provider::GroupPlayback);
    }
    bool remoteControl() const
    {
        return m_flags.testFlag(Provider::RemoteControl);
    }
    bool streamQuality() const
    {
        return m_flags.testFlag(Provider::StreamQuality);
    }
    bool trickplay() const
    {
        return m_flags.testFlag(Provider::Trickplay);
    }
    bool speedTest() const
    {
        return m_flags.testFlag(Provider::SpeedTest);
    }

signals:
    void changed();

private:
    Provider::Capabilities m_flags;
};

} // namespace Spool
