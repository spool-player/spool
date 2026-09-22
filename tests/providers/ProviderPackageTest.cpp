#include "provider/ProviderPackage.h"
#include "TestMain.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

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

QByteArray makeStoredZip(const QMap<QString, QByteArray>& entries)
{
    QByteArray buffer;
    struct EntryMeta {
        QString name;
        uint32_t offset;
        uint32_t crc;
        uint32_t size;
    };
    QList<EntryMeta> metas;

    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        const QString name = it.key();
        const QByteArray data = it.value();
        const uint32_t crc = JellyfinNative::ProviderPackage::calculateCrc32(
            reinterpret_cast<const uint8_t *>(data.constData()), data.size());
        const uint32_t offset = static_cast<uint32_t>(buffer.size());

        ZipLocalHeader local {};
        local.signature = 0x04034b50;
        local.versionNeeded = 20;
        local.method = 0; // Stored
        local.crc32 = crc;
        local.compressedSize = static_cast<uint32_t>(data.size());
        local.uncompressedSize = static_cast<uint32_t>(data.size());
        local.filenameLen = static_cast<uint16_t>(name.toUtf8().size());
        local.extraLen = 0;

        buffer.append(reinterpret_cast<const char *>(&local), sizeof(local));
        buffer.append(name.toUtf8());
        buffer.append(data);

        metas.append({ name, offset, crc, static_cast<uint32_t>(data.size()) });
    }

    const uint32_t cdOffset = static_cast<uint32_t>(buffer.size());
    for (const auto& meta : metas) {
        ZipCDHeader cd {};
        cd.signature = 0x02014b50;
        cd.versionMadeBy = 20;
        cd.versionNeeded = 20;
        cd.flags = 0;
        cd.method = 0;
        cd.crc32 = meta.crc;
        cd.compressedSize = meta.size;
        cd.uncompressedSize = meta.size;
        cd.filenameLen = static_cast<uint16_t>(meta.name.toUtf8().size());
        cd.extraLen = 0;
        cd.commentLen = 0;
        cd.diskNumStart = 0;
        cd.internalAttr = 0;
        cd.externalAttr = 0;
        cd.localHeaderOffset = meta.offset;

        buffer.append(reinterpret_cast<const char *>(&cd), sizeof(cd));
        buffer.append(meta.name.toUtf8());
    }
    const uint32_t cdSize = static_cast<uint32_t>(buffer.size() - cdOffset);

    ZipEOCD eocd {};
    eocd.signature = 0x06054b50;
    eocd.diskNumber = 0;
    eocd.cdStartDisk = 0;
    eocd.numEntriesThisDisk = static_cast<uint16_t>(metas.size());
    eocd.numEntries = static_cast<uint16_t>(metas.size());
    eocd.cdSize = cdSize;
    eocd.cdOffset = cdOffset;
    eocd.commentLen = 0;

    buffer.append(reinterpret_cast<const char *>(&eocd), sizeof(eocd));
    return buffer;
}

} // namespace

JELLYFIN_TEST_MAIN("provider-package-unpack")
{
    QCoreApplication app(argc, argv);
    using namespace JellyfinNative;

    // Test 1: Valid package validation and extraction
    {
        const QByteArray manifest = R"({
            "format": 1,
            "id": "spool.test",
            "version": "1.0.0",
            "api": "0.1",
            "entry": "logic/provider.mjs"
        })";
        const QByteArray logic = "export function init() { return true; }";

        QMap<QString, QByteArray> entries;
        entries.insert(QStringLiteral("manifest.json"), manifest);
        entries.insert(QStringLiteral("logic/provider.mjs"), logic);

        const QByteArray zipData = makeStoredZip(entries);
        QString errorMsg;
        auto package = ProviderPackage::parseAndValidate(zipData, &errorMsg);
        require(package.has_value(), "valid package parsed successfully");
        require(package->id == QStringLiteral("spool.test"), "package ID extracted");
        require(package->version == QStringLiteral("1.0.0"), "package version extracted");
        require(package->entryPoint == QStringLiteral("logic/provider.mjs"), "entrypoint extracted");
        require(package->files.size() == 2, "package files map populated");

        QTemporaryDir tempDir;
        require(tempDir.isValid(), "temporary directory created");
        const QString installedPath = ProviderPackage::install(*package, tempDir.path(), &errorMsg);
        require(!installedPath.isEmpty(), "package installed successfully");
        require(QFile::exists(installedPath), "installed entrypoint file exists");
    }

    // Test 2: Path traversal attack prevention
    {
        const QByteArray manifest = R"({
            "format": 1,
            "id": "spool.evil",
            "version": "1.0.0",
            "api": "0.1",
            "entry": "logic/provider.mjs"
        })";
        QMap<QString, QByteArray> entries;
        entries.insert(QStringLiteral("manifest.json"), manifest);
        entries.insert(QStringLiteral("../../../evil.mjs"), "alert('evil')");

        const QByteArray zipData = makeStoredZip(entries);
        QString errorMsg;
        auto package = ProviderPackage::parseAndValidate(zipData, &errorMsg);
        require(!package.has_value(), "path traversal package rejected");
    }

    // Test 3: Disallowed executable extension rejection
    {
        const QByteArray manifest = R"({
            "format": 1,
            "id": "spool.binary",
            "version": "1.0.0",
            "api": "0.1",
            "entry": "logic/provider.mjs"
        })";
        QMap<QString, QByteArray> entries;
        entries.insert(QStringLiteral("manifest.json"), manifest);
        entries.insert(QStringLiteral("payload.exe"), "fake executable");

        const QByteArray zipData = makeStoredZip(entries);
        QString errorMsg;
        auto package = ProviderPackage::parseAndValidate(zipData, &errorMsg);
        require(!package.has_value(), "disallowed file extension rejected");
    }

    // Test 4: Native binary payload rejection (ELF header)
    {
        const QByteArray manifest = R"({
            "format": 1,
            "id": "spool.native",
            "version": "1.0.0",
            "api": "0.1",
            "entry": "logic/provider.mjs"
        })";
        QByteArray elfData;
        elfData.append("\x7f\x45\x4c\x46", 4); // \x7fELF
        elfData.append(32, '\0');

        QMap<QString, QByteArray> entries;
        entries.insert(QStringLiteral("manifest.json"), manifest);
        entries.insert(QStringLiteral("logic/provider.mjs"), elfData);

        const QByteArray zipData = makeStoredZip(entries);
        QString errorMsg;
        auto package = ProviderPackage::parseAndValidate(zipData, &errorMsg);
        require(!package.has_value(), "ELF binary payload rejected");
    }

    // Test 5: Missing manifest rejection
    {
        QMap<QString, QByteArray> entries;
        entries.insert(QStringLiteral("logic/provider.mjs"), "export function init() {}");

        const QByteArray zipData = makeStoredZip(entries);
        QString errorMsg;
        auto package = ProviderPackage::parseAndValidate(zipData, &errorMsg);
        require(!package.has_value(), "archive without manifest rejected");
    }

    std::cout << "provider package test ok\n";
    return 0;
}
