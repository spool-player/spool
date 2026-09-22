#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <optional>

namespace JellyfinNative {

struct ProviderPackageContents {
    QString id;
    QString version;
    QString entryPoint;
    QMap<QString, QByteArray> files;
};

class ProviderPackage final {
public:
    // Safely validates and decompresses an in-memory ZIP package without
    // executing any code. Enforces size limits, path sanitization, allowed
    // extensions, executable-payload rejection, and manifest schema rules.
    static std::optional<ProviderPackageContents> parseAndValidate(
        const QByteArray& zipBytes, QString *errorMessage = nullptr);

    // Extracts validated package files into targetDirectory/package.id,
    // creating parent directories as necessary. Returns the full local path
    // of the JavaScript entry point on success, or an empty string on error.
    static QString install(
        const ProviderPackageContents& package, const QString& targetDirectory, QString *errorMessage = nullptr);

    // Calculates standard IEEE 802.3 32-bit CRC.
    static uint32_t calculateCrc32(const uint8_t *data, size_t length);
};

} // namespace JellyfinNative
