#include "ProviderRegistry.h"

#include <algorithm>

namespace JellyfinNative {

ProviderCapabilities::ProviderCapabilities(QObject *parent)
    : QObject(parent)
{
}

void ProviderCapabilities::setFlags(Provider::Capabilities flags)
{
    if (m_flags == flags)
        return;
    m_flags = flags;
    emit changed();
}

ProviderRegistry::ProviderRegistry(QObject *parent)
    : QObject(parent)
{
}

void ProviderRegistry::add(Provider *provider)
{
    if (!provider || std::find(m_providers.begin(), m_providers.end(), provider) != m_providers.end())
        return;
    m_providers.push_back(provider);
}

Provider *ProviderRegistry::provider(const QString& id) const
{
    const auto it = std::find_if(
        m_providers.begin(), m_providers.end(), [&id](const Provider *candidate) { return candidate->id() == id; });
    return it == m_providers.end() ? nullptr : *it;
}

bool ProviderRegistry::setActive(const QString& id)
{
    Provider *candidate = provider(id);
    if (!candidate)
        return false;
    setActive(candidate);
    return true;
}

void ProviderRegistry::setActive(Provider *provider)
{
    if (m_active == provider)
        return;
    add(provider);
    QObject::disconnect(m_activeCapabilities);
    m_active = provider;
    if (m_active) {
        m_activeCapabilities
            = connect(m_active, &Provider::capabilitiesChanged, this, &ProviderRegistry::refreshCapabilities);
    }
    refreshCapabilities();
    emit activeChanged();
}

void ProviderRegistry::refreshCapabilities()
{
    m_capabilities.setFlags(m_active ? m_active->capabilities() : Provider::Capabilities {});
}

} // namespace JellyfinNative
