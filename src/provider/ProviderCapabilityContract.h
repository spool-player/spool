#pragma once

#include <QStringList>
#include <QVariantMap>

namespace Spool::ProviderCapabilityContract {

// The current package schema has a bounded, closed set of capability names.
// Declarations are true flags; account offers must contain actual booleans.
// Malformed declarations/offers throw invalid_capabilities.
QVariantMap declarations(const QStringList& names);
QVariantMap decodeOffers(const QVariant& value);
QVariantMap intersect(const QVariantMap& declarations, const QVariantMap& offers);

// Empty means a lifecycle, baseline catalogue, or provider-custom operation.
// Optional payload features still require their own runtime permission checks.
QString operationCapability(const QString& operation);

} // namespace Spool::ProviderCapabilityContract
