#include "ProviderCapabilityContract.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace Spool::ProviderCapabilityContract {
namespace {
    constexpr const char *kCapabilities[] = {
        "search",
        "userState",
        "reporting",
        "segments",
        "groupPlayback",
        "remoteControl",
        "streamQuality",
        "trickplay",
        "speedTest",
        "downloads",
        "downloadTranscode",
        "discovery",
        "artworkOwners",
        "suggestions",
        "playbackPreferences",
        "settingsStorage",
        "itemActions",
        "collectionEditing",
        "playbackQueueReporting",
        "remoteTargets",
        "httpMetadata",
        "originGrants",
        "lanProbe",
        "accountActivation",
    };

    bool known(const QString& name)
    {
        return std::any_of(std::begin(kCapabilities), std::end(kCapabilities),
            [&name](const char *candidate) { return name == QLatin1String(candidate); });
    }
} // namespace

QVariantMap declarations(const QStringList& names)
{
    if (names.size() > static_cast<qsizetype>(std::size(kCapabilities)))
        throw std::runtime_error("invalid_capabilities");
    QVariantMap result;
    for (const QString& name : names) {
        if (!known(name) || result.contains(name))
            throw std::runtime_error("invalid_capabilities");
        result.insert(name, true);
    }
    return result;
}

QVariantMap decodeOffers(const QVariant& value)
{
    if (value.metaType().id() != QMetaType::QVariantMap)
        throw std::runtime_error("invalid_capabilities");
    const QVariantMap offers = value.toMap();
    if (offers.size() > static_cast<qsizetype>(std::size(kCapabilities)))
        throw std::runtime_error("invalid_capabilities");
    for (auto it = offers.cbegin(); it != offers.cend(); ++it) {
        if (!known(it.key()) || it.value().metaType().id() != QMetaType::Bool)
            throw std::runtime_error("invalid_capabilities");
    }
    return offers;
}

QVariantMap intersect(const QVariantMap& declarations, const QVariantMap& offers)
{
    const QVariantMap decoded = decodeOffers(offers);
    QVariantMap result;
    for (auto it = declarations.cbegin(); it != declarations.cend(); ++it) {
        if (it.value().metaType().id() == QMetaType::Bool && it.value().toBool() && decoded.value(it.key()).toBool())
            result.insert(it.key(), true);
    }
    return result;
}

QString operationCapability(const QString& operation)
{
    struct Mapping {
        const char *operation;
        const char *capability;
    };
    static constexpr Mapping mappings[] = {
        { "search", "search" },
        { "favorite", "userState" },
        { "played", "userState" },
        { "progress", "userState" },
        { "report", "reporting" },
        { "segments", "segments" },
        { "groups", "groupPlayback" },
        { "groupCreate", "groupPlayback" },
        { "groupJoin", "groupPlayback" },
        { "groupLeave", "groupPlayback" },
        { "groupSend", "groupPlayback" },
        { "clock", "groupPlayback" },
        { "download", "downloads" },
        { "discover", "discovery" },
        { "speedTest", "speedTest" },
        { "suggestions", "suggestions" },
        { "preferencesRead", "playbackPreferences" },
        { "preferencesWrite", "playbackPreferences" },
        { "dataInfo", "settingsStorage" },
        { "dataRead", "settingsStorage" },
        { "dataWrite", "settingsStorage" },
        { "dataDelete", "settingsStorage" },
        { "itemActions", "itemActions" },
        { "collectionInfo", "collectionEditing" },
        { "collectionEntries", "collectionEditing" },
        { "collectionRemove", "collectionEditing" },
        { "collectionMove", "collectionEditing" },
        { "remoteTargets", "remoteTargets" },
        { "remoteConnect", "remoteTargets" },
        { "remoteState", "remoteTargets" },
        { "remoteQueue", "remoteTargets" },
        { "remoteCommand", "remoteTargets" },
        { "discoverMore", "lanProbe" },
        { "activate", "accountActivation" },
    };
    for (const Mapping& mapping : mappings) {
        if (operation == QLatin1String(mapping.operation))
            return QString::fromLatin1(mapping.capability);
    }
    return {};
}

} // namespace Spool::ProviderCapabilityContract
