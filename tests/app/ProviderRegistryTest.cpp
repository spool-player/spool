#include "provider/ProviderRegistry.h"
#include "provider/PlaybackSource.h"
#include "provider/Provider.h"

#include "TestMain.h"

#include <QCoreApplication>
#include <QMetaProperty>
#include <QStringList>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class StubProvider final : public JellyfinNative::Provider {
public:
    StubProvider(QString id, Capabilities capabilities, QObject *parent = nullptr)
        : Provider(parent)
        , m_id(std::move(id))
        , m_capabilities(capabilities)
    {
    }

    QString id() const override
    {
        return m_id;
    }
    QString displayName() const override
    {
        return m_id;
    }
    Capabilities capabilities() const override
    {
        return m_capabilities;
    }
    JellyfinNative::PlaybackSource *playback() override
    {
        return nullptr;
    }
    void registerQmlSingletons() override { }

    void setCapabilities(Capabilities capabilities)
    {
        m_capabilities = capabilities;
        emit capabilitiesChanged();
    }

private:
    QString m_id;
    Capabilities m_capabilities;
};

// The eleven names shared QML gates on, in the order the flags are declared.
const QStringList kCapabilityNames { QStringLiteral("auth"), QStringLiteral("discovery"), QStringLiteral("search"),
    QStringLiteral("userItemState"), QStringLiteral("playbackReporting"), QStringLiteral("segments"),
    QStringLiteral("libraryManagement"), QStringLiteral("syncPlay"), QStringLiteral("remoteControl"),
    QStringLiteral("quickConnect"), QStringLiteral("peerRelay") };

bool propertyValue(const QObject& object, const QString& name)
{
    const QVariant value = object.property(name.toLatin1().constData());
    require(value.isValid() && value.typeId() == QMetaType::Bool, "capability property is a bool");
    return value.toBool();
}

} // namespace

JELLYFIN_TEST_MAIN("provider-registry")
{
    QCoreApplication app(argc, argv);
    using JellyfinNative::Provider;

    JellyfinNative::ProviderRegistry registry;
    JellyfinNative::ProviderCapabilities *capabilities = registry.capabilities();
    require(registry.active() == nullptr, "no provider is active until one is chosen");
    for (const QString& name : kCapabilityNames)
        require(!propertyValue(*capabilities, name), "every capability is off with no active provider");

    // Every flag maps to the property of the same name, and only that one.
    Provider::Capabilities all;
    const QList<Provider::Capability> flags { Provider::Auth, Provider::Discovery, Provider::Search,
        Provider::UserItemState, Provider::PlaybackReporting, Provider::Segments, Provider::LibraryManagement,
        Provider::SyncPlay, Provider::RemoteControl, Provider::QuickConnect, Provider::PeerRelay };
    require(flags.size() == kCapabilityNames.size(), "one flag per capability name");
    StubProvider single(QStringLiteral("single"), {});
    registry.setActive(&single);
    for (qsizetype index = 0; index < flags.size(); ++index) {
        single.setCapabilities(flags.at(index));
        for (qsizetype other = 0; other < kCapabilityNames.size(); ++other) {
            require(propertyValue(*capabilities, kCapabilityNames.at(other)) == (other == index),
                "a flag turns on exactly the property of its own name");
        }
        all |= flags.at(index);
    }

    StubProvider jellyfin(QStringLiteral("jellyfin"), all);
    StubProvider local(QStringLiteral("local"), Provider::Search);
    registry.add(&jellyfin);
    registry.add(&local);
    registry.add(&jellyfin);
    require(registry.providers().size() == 3, "adding a provider twice registers it once");
    require(registry.provider(QStringLiteral("local")) == &local, "providers are found by id");
    require(!registry.setActive(QStringLiteral("plex")), "an unknown id leaves the active provider alone");
    require(registry.active() == &single, "an unknown id leaves the active provider alone");

    int activeChanged = 0;
    int capabilitiesChanged = 0;
    QObject::connect(&registry, &JellyfinNative::ProviderRegistry::activeChanged, &app, [&] { ++activeChanged; });
    QObject::connect(
        capabilities, &JellyfinNative::ProviderCapabilities::changed, &app, [&] { ++capabilitiesChanged; });
    require(registry.setActive(QStringLiteral("jellyfin")), "a known id activates its provider");
    require(registry.active() == &jellyfin, "the named provider is active");
    require(activeChanged == 1, "activation notifies once");
    require(capabilitiesChanged == 1, "the capability change notifies once");
    for (const QString& name : kCapabilityNames)
        require(propertyValue(*capabilities, name), "every capability is on for the full provider");

    registry.setActive(&local);
    require(activeChanged == 2, "switching providers notifies again");
    require(capabilitiesChanged == 2, "switching providers refreshes the flags");
    require(propertyValue(*capabilities, QStringLiteral("search")), "the local provider searches");
    require(!propertyValue(*capabilities, QStringLiteral("syncPlay")), "the local provider has no SyncPlay");

    // A change on the previous provider no longer reaches the flags; one on
    // the active provider does.
    jellyfin.setCapabilities({});
    require(capabilitiesChanged == 2, "the previous provider is disconnected");
    local.setCapabilities(Provider::Search | Provider::Segments);
    require(capabilitiesChanged == 3, "the active provider drives the flags");
    require(propertyValue(*capabilities, QStringLiteral("segments")), "the new flag is visible");
    local.setCapabilities(Provider::Search | Provider::Segments);
    require(capabilitiesChanged == 3, "an unchanged flag set does not notify");

    registry.setActive(nullptr);
    require(registry.active() == nullptr, "the active provider can be cleared");
    for (const QString& name : kCapabilityNames)
        require(!propertyValue(*capabilities, name), "clearing the active provider clears every flag");

    // The QML singleton contract: exactly these property names, all booleans.
    const QMetaObject *meta = capabilities->metaObject();
    QStringList declared;
    for (int index = meta->propertyOffset(); index < meta->propertyCount(); ++index)
        declared.append(QString::fromLatin1(meta->property(index).name()));
    require(declared == kCapabilityNames, "ProviderCapabilities declares exactly the eleven contract names in order");

    std::cout << "provider registry ok\n";
    return 0;
}
