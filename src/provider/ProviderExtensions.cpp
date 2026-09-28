#include "ProviderExtensions.h"

#include <QRegularExpression>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace Spool::ProviderExtensions {
namespace {
    constexpr const char *kVersionOne[] = {
        "spool.artwork-owners",
        "spool.speed-test",
        "spool.suggestions",
        "spool.playback-preferences",
        "spool.settings-storage",
        "spool.item-actions",
        "spool.collection-editing",
        "spool.playback-queue-reporting",
        "spool.remote-targets",
        "spool.http-metadata",
        "spool.origin-grants",
        "spool.lan-probe",
        "spool.account-activation",
    };

    int wireMajor(const QVariant& value)
    {
        switch (value.metaType().id()) {
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
        case QMetaType::Double:
        case QMetaType::Float:
            break;
        default:
            throw std::runtime_error("invalid_extensions");
        }
        const double number = value.toDouble();
        if (!std::isfinite(number) || number < 1 || number > std::numeric_limits<int>::max()
            || std::floor(number) != number)
            throw std::runtime_error("invalid_extensions");
        return static_cast<int>(number);
    }
} // namespace

QVariantMap decode(const QVariant& value)
{
    if (value.metaType().id() != QMetaType::QVariantMap)
        throw std::runtime_error("invalid_extensions");
    const QVariantMap map = value.toMap();
    if (map.size() > 32)
        throw std::runtime_error("invalid_extensions");
    static const QRegularExpression idPattern(QStringLiteral("\\A[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+\\z"));
    QVariantMap decoded;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        if (it.key().size() > 128 || !idPattern.match(it.key()).hasMatch())
            throw std::runtime_error("invalid_extensions");
        decoded.insert(it.key(), wireMajor(it.value()));
    }
    return decoded;
}

QVariantMap supported(const QVariantMap& declarations)
{
    const QVariantMap decoded = decode(declarations);
    QVariantMap result;
    for (const char *id : kVersionOne) {
        const auto declared = decoded.constFind(QLatin1String(id));
        if (declared != decoded.cend() && declared.value().toInt() == 1)
            result.insert(declared.key(), 1);
    }
    return result;
}

QVariantMap intersect(const QVariantMap& declarations, const QVariantMap& offers)
{
    QVariantMap result = supported(declarations);
    const QVariantMap decoded = decode(offers);
    for (auto it = result.begin(); it != result.end();) {
        if (decoded.value(it.key()).toInt() != it.value().toInt())
            it = result.erase(it);
        else
            ++it;
    }
    return result;
}

QString operationExtension(const QString& operation)
{
    struct Mapping {
        const char *operation;
        const char *extension;
    };
    static constexpr Mapping mappings[] = {
        { "speedTest", "spool.speed-test" },
        { "suggestions", "spool.suggestions" },
        { "preferencesRead", "spool.playback-preferences" },
        { "preferencesWrite", "spool.playback-preferences" },
        { "dataInfo", "spool.settings-storage" },
        { "dataRead", "spool.settings-storage" },
        { "dataWrite", "spool.settings-storage" },
        { "dataDelete", "spool.settings-storage" },
        { "itemActions", "spool.item-actions" },
        { "collectionInfo", "spool.collection-editing" },
        { "collectionEntries", "spool.collection-editing" },
        { "collectionRemove", "spool.collection-editing" },
        { "collectionMove", "spool.collection-editing" },
        { "remoteTargets", "spool.remote-targets" },
        { "remoteConnect", "spool.remote-targets" },
        { "remoteState", "spool.remote-targets" },
        { "remoteQueue", "spool.remote-targets" },
        { "remoteCommand", "spool.remote-targets" },
        { "discoverMore", "spool.lan-probe" },
        { "activate", "spool.account-activation" },
    };
    for (const Mapping& mapping : mappings) {
        if (operation == QLatin1String(mapping.operation))
            return QString::fromLatin1(mapping.extension);
    }
    return {};
}

} // namespace Spool::ProviderExtensions
