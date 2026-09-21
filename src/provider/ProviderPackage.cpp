#include "ProviderPackage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <cstring>
#include <zlib.h>

namespace JellyfinNative {

namespace {
    constexpr qint64 kMaxArchiveBytes = 16 * 1024 * 1024;
    constexpr qint64 kMaxExpandedBytes = 32 * 1024 * 1024;
    constexpr qint64 kMaxFileBytes = 8 * 1024 * 1024;
    constexpr int kMaxFiles = 512;

#pragma pack(push, 1)
    struct ZipEOCD {
        uint32_t signature;
        uint16_t diskNumber;
        uint16_t cdStartDisk;
        uint16_t numEntriesThisDisk;
        uint16_t numEntries;
        uint32_t cdSize;
        uint32_t cdOffset;
        uint16_t commentLen;
    };

    struct ZipCDHeader {
        uint32_t signature;
        uint16_t versionMadeBy;
        uint16_t versionNeeded;
        uint16_t flags;
        uint16_t method;
        uint16_t modTime;
        uint16_t modDate;
        uint32_t crc32;
        uint32_t compressedSize;
        uint32_t uncompressedSize;
        uint16_t filenameLen;
        uint16_t extraLen;
        uint16_t commentLen;
        uint16_t diskNumStart;
        uint16_t internalAttr;
        uint32_t externalAttr;
        uint32_t localHeaderOffset;
    };

    struct ZipLocalHeader {
        uint32_t signature;
        uint16_t versionNeeded;
        uint16_t flags;
        uint16_t method;
        uint16_t modTime;
        uint16_t modDate;
        uint32_t crc32;
        uint32_t compressedSize;
        uint32_t uncompressedSize;
        uint16_t filenameLen;
        uint16_t extraLen;
    };
#pragma pack(pop)

    bool isValidPackagePath(const QString& path)
    {
        if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || path.contains(QLatin1Char('\\'))
            || path.contains(QLatin1Char(':')) || path.contains(QStringLiteral(".."))) {
            return false;
        }
        static const QSet<QString> allowedExtensions = { QStringLiteral("mjs"), QStringLiteral("js"),
            QStringLiteral("qml"), QStringLiteral("json"), QStringLiteral("png"), QStringLiteral("jpg"),
            QStringLiteral("jpeg"), QStringLiteral("svg"), QStringLiteral("webp"), QStringLiteral("ttf"),
            QStringLiteral("otf"), QStringLiteral("txt"), QStringLiteral("map") };
        const QString ext = QFileInfo(path).suffix().toLower();
        if (ext.isEmpty()) {
            const QString fileName = QFileInfo(path).fileName();
            if (fileName != QStringLiteral("LICENSE") && fileName != QStringLiteral("NOTICE"))
                return false;
        } else if (!allowedExtensions.contains(ext)) {
            return false;
        }

        const auto parts = path.split(QLatin1Char('/'));
        for (const auto& part : parts) {
            if (part.isEmpty() || part == QStringLiteral(".") || part.endsWith(QLatin1Char('.'))
                || part.endsWith(QLatin1Char(' '))) {
                return false;
            }
        }
        return true;
    }

    bool hasNativeMagic(const QByteArray& data)
    {
        if (data.size() < 4)
            return false;
        const auto *bytes = reinterpret_cast<const unsigned char *>(data.constData());
        // ELF: \x7fELF
        if (bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F')
            return true;
        // DOS / PE: MZ
        if (bytes[0] == 'M' && bytes[1] == 'Z')
            return true;
        // Mach-O
        if ((bytes[0] == 0xca && bytes[1] == 0xfe && bytes[2] == 0xba && bytes[3] == 0xbe)
            || (bytes[0] == 0xce && bytes[1] == 0xfa && bytes[2] == 0xed && bytes[3] == 0xfe)
            || (bytes[0] == 0xcf && bytes[1] == 0xfa && bytes[2] == 0xed && bytes[3] == 0xfe))
            return true;
        // WebAssembly: \0asm
        if (bytes[0] == 0x00 && bytes[1] == 'a' && bytes[2] == 's' && bytes[3] == 'm')
            return true;
        return false;
    }

    void setError(QString *errorOut, const QString& message)
    {
        if (errorOut)
            *errorOut = message;
    }
} // namespace

std::optional<ProviderPackageContents> ProviderPackage::parseAndValidate(
    const QByteArray& zipBytes, QString *errorMessage)
{
    if (zipBytes.isEmpty()) {
        setError(errorMessage, QStringLiteral("Package archive is empty"));
        return std::nullopt;
    }
    if (zipBytes.size() > kMaxArchiveBytes) {
        setError(errorMessage, QStringLiteral("Package archive exceeds maximum size limit (16MB)"));
        return std::nullopt;
    }

    if (zipBytes.size() < static_cast<qint64>(sizeof(ZipEOCD))) {
        setError(errorMessage, QStringLiteral("Archive is too small to be a valid ZIP file"));
        return std::nullopt;
    }

    // Locate End of Central Directory record
    const auto *data = reinterpret_cast<const uint8_t *>(zipBytes.constData());
    const qint64 dataSize = zipBytes.size();
    const ZipEOCD *eocd = nullptr;
    const uint8_t *search = data + dataSize - sizeof(ZipEOCD);
    const uint8_t *minSearch = data + std::max<qint64>(0, dataSize - sizeof(ZipEOCD) - 65536);

    while (search >= minSearch) {
        if (*reinterpret_cast<const uint32_t *>(search) == 0x06054b50) {
            eocd = reinterpret_cast<const ZipEOCD *>(search);
            break;
        }
        --search;
    }

    if (!eocd) {
        setError(errorMessage, QStringLiteral("End of central directory not found in ZIP"));
        return std::nullopt;
    }

    if (eocd->numEntries > kMaxFiles) {
        setError(errorMessage, QStringLiteral("Package contains too many files"));
        return std::nullopt;
    }

    if (eocd->cdOffset + eocd->cdSize > static_cast<uint64_t>(dataSize)) {
        setError(errorMessage, QStringLiteral("Corrupt ZIP central directory offset"));
        return std::nullopt;
    }

    ProviderPackageContents package;
    qint64 totalExpandedBytes = 0;
    QSet<QString> caseFoldedNames;

    const uint8_t *cdPtr = data + eocd->cdOffset;
    for (int i = 0; i < eocd->numEntries; ++i) {
        if (cdPtr + sizeof(ZipCDHeader) > data + dataSize) {
            setError(errorMessage, QStringLiteral("Corrupt central directory record"));
            return std::nullopt;
        }
        const auto *cd = reinterpret_cast<const ZipCDHeader *>(cdPtr);
        if (cd->signature != 0x02014b50) {
            setError(errorMessage, QStringLiteral("Invalid central directory header signature"));
            return std::nullopt;
        }

        if (cdPtr + sizeof(ZipCDHeader) + cd->filenameLen + cd->extraLen + cd->commentLen > data + dataSize) {
            setError(errorMessage, QStringLiteral("Truncated central directory entry"));
            return std::nullopt;
        }

        const QString entryPath
            = QString::fromUtf8(reinterpret_cast<const char *>(cdPtr + sizeof(ZipCDHeader)), cd->filenameLen);

        // Skip directory-only records if empty
        if (entryPath.endsWith(QLatin1Char('/'))) {
            cdPtr += sizeof(ZipCDHeader) + cd->filenameLen + cd->extraLen + cd->commentLen;
            continue;
        }

        if (!isValidPackagePath(entryPath)) {
            setError(errorMessage, QStringLiteral("Invalid or forbidden path in package: %1").arg(entryPath));
            return std::nullopt;
        }

        const QString folded = entryPath.toCaseFolded();
        if (caseFoldedNames.contains(folded)) {
            setError(errorMessage, QStringLiteral("Case-colliding paths detected: %1").arg(entryPath));
            return std::nullopt;
        }
        caseFoldedNames.insert(folded);

        if (cd->uncompressedSize > kMaxFileBytes) {
            setError(errorMessage, QStringLiteral("File %1 exceeds maximum allowed size (8MB)").arg(entryPath));
            return std::nullopt;
        }

        totalExpandedBytes += cd->uncompressedSize;
        if (totalExpandedBytes > kMaxExpandedBytes) {
            setError(errorMessage, QStringLiteral("Package total expanded size exceeds limit (32MB)"));
            return std::nullopt;
        }

        if (cd->localHeaderOffset + sizeof(ZipLocalHeader) > static_cast<uint64_t>(dataSize)) {
            setError(errorMessage, QStringLiteral("Invalid local header offset for %1").arg(entryPath));
            return std::nullopt;
        }

        const auto *loc = reinterpret_cast<const ZipLocalHeader *>(data + cd->localHeaderOffset);
        if (loc->signature != 0x04034b50) {
            setError(errorMessage, QStringLiteral("Invalid local header signature for %1").arg(entryPath));
            return std::nullopt;
        }

        const uint8_t *fileData
            = data + cd->localHeaderOffset + sizeof(ZipLocalHeader) + loc->filenameLen + loc->extraLen;
        if (fileData + cd->compressedSize > data + dataSize) {
            setError(errorMessage, QStringLiteral("Truncated file data for %1").arg(entryPath));
            return std::nullopt;
        }

        QByteArray uncompressed;
        uncompressed.resize(cd->uncompressedSize);

        if (cd->method == 0) {
            // Stored
            if (cd->compressedSize != cd->uncompressedSize) {
                setError(errorMessage, QStringLiteral("Mismatched stored size for %1").arg(entryPath));
                return std::nullopt;
            }
            std::memcpy(uncompressed.data(), fileData, cd->uncompressedSize);
        } else if (cd->method == 8) {
            // Deflated
            z_stream strm = {};
            if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
                setError(errorMessage, QStringLiteral("Failed to initialize decompression"));
                return std::nullopt;
            }
            strm.next_in = const_cast<uint8_t *>(fileData);
            strm.avail_in = cd->compressedSize;
            strm.next_out = reinterpret_cast<uint8_t *>(uncompressed.data());
            strm.avail_out = uncompressed.size();
            const int ret = inflate(&strm, Z_FINISH);
            inflateEnd(&strm);
            if (ret != Z_STREAM_END) {
                setError(errorMessage, QStringLiteral("Corrupt compressed stream for %1").arg(entryPath));
                return std::nullopt;
            }
        } else {
            setError(errorMessage, QStringLiteral("Unsupported compression method for %1").arg(entryPath));
            return std::nullopt;
        }

        if (hasNativeMagic(uncompressed)) {
            setError(errorMessage, QStringLiteral("Native executable binary rejected in %1").arg(entryPath));
            return std::nullopt;
        }

        package.files.insert(entryPath, uncompressed);
        cdPtr += sizeof(ZipCDHeader) + cd->filenameLen + cd->extraLen + cd->commentLen;
    }

    if (!package.files.contains(QStringLiteral("manifest.json"))) {
        setError(errorMessage, QStringLiteral("Package is missing manifest.json"));
        return std::nullopt;
    }

    const QJsonDocument manifestDoc = QJsonDocument::fromJson(package.files.value(QStringLiteral("manifest.json")));
    if (!manifestDoc.isObject()) {
        setError(errorMessage, QStringLiteral("manifest.json is not a valid JSON object"));
        return std::nullopt;
    }
    const QJsonObject manifest = manifestDoc.object();

    if (manifest.value(QStringLiteral("format")).toInt() != 1
        || manifest.value(QStringLiteral("api")).toString() != QStringLiteral("0.1")) {
        setError(errorMessage, QStringLiteral("Unsupported manifest format or API revision"));
        return std::nullopt;
    }

    static const QRegularExpression idPattern(QStringLiteral("^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+$"));
    package.id = manifest.value(QStringLiteral("id")).toString();
    if (!idPattern.match(package.id).hasMatch()) {
        setError(errorMessage, QStringLiteral("Invalid provider ID in manifest"));
        return std::nullopt;
    }

    package.version = manifest.value(QStringLiteral("version")).toString();
    if (package.version.isEmpty()) {
        setError(errorMessage, QStringLiteral("Missing version in manifest"));
        return std::nullopt;
    }

    package.entryPoint = manifest.value(QStringLiteral("entry")).toString();
    if (!package.entryPoint.endsWith(QStringLiteral(".mjs")) || !package.files.contains(package.entryPoint)) {
        setError(errorMessage, QStringLiteral("Missing or invalid JavaScript entry point in manifest"));
        return std::nullopt;
    }

    return package;
}

QString ProviderPackage::install(
    const ProviderPackageContents& package, const QString& targetDirectory, QString *errorMessage)
{
    if (package.id.isEmpty() || package.files.isEmpty()) {
        setError(errorMessage, QStringLiteral("Invalid package to install"));
        return QString();
    }

    const QDir baseDir(targetDirectory);
    const QString destPath = baseDir.filePath(package.id);
    const QDir destDir(destPath);
    if (!destDir.mkpath(QStringLiteral("."))) {
        setError(errorMessage, QStringLiteral("Could not create provider directory: %1").arg(destPath));
        return QString();
    }

    for (auto it = package.files.cbegin(); it != package.files.cend(); ++it) {
        const QString relativeFilePath = it.key();
        const QString targetFilePath = destDir.filePath(relativeFilePath);
        const QFileInfo fileInfo(targetFilePath);
        destDir.mkpath(fileInfo.path());

        QFile file(targetFilePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            setError(errorMessage, QStringLiteral("Failed to write package file: %1").arg(targetFilePath));
            return QString();
        }
        file.write(it.value());
        file.close();
    }

    return destDir.filePath(package.entryPoint);
}

} // namespace JellyfinNative
