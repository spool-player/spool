#pragma once

#include <QString>
#include <QVariantMap>

namespace Spool::ProviderExtensions {

// Wire majors are exact positive integers, not compatibility ranges. Unknown
// well-formed declarations are retained by decode but never grant host support.
// Throws invalid_extensions for a malformed map (including an explicit null).
QVariantMap decode(const QVariant& value);
QVariantMap supported(const QVariantMap& declarations);
QVariantMap intersect(const QVariantMap& declarations, const QVariantMap& offers);

// Empty means a baseline/custom operation. Existing report/runItemAction stay
// baseline operations even when optional payloads or action discovery are used.
QString operationExtension(const QString& operation);

} // namespace Spool::ProviderExtensions
