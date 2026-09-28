#pragma once

#include <QVariantMap>

namespace Spool::ProviderExtensionData {

inline constexpr int MaximumBytes = 64 * 1024;

struct StorageInfo {
    int maxBytes = MaximumBytes;
    bool conditionalWrites = false;
};

StorageInfo storageInfo(const QVariantMap& result);
// Validates exact JSON types, nesting and compact UTF-8 wire size, including scalar/null documents.
void validateValue(const QVariant& value, int maxBytes, const char *error);
QVariantMap preferenceArguments(const QString& operation, const QVariantMap& arguments);
QVariantMap preferenceResult(const QString& operation, const QVariantMap& result);
void requireWritable(const QVariantMap& values, const QVariantMap& preferences);
void storageArguments(const QString& operation, const QVariantMap& arguments, const StorageInfo *info = nullptr);
QVariantMap storageResult(const QString& operation, const QVariantMap& result, const StorageInfo& info);

} // namespace Spool::ProviderExtensionData
