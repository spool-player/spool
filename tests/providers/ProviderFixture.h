#pragma once

#include "provider/ProviderPackage.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace ProviderFixture {

using Entries = std::vector<std::pair<QByteArray, QByteArray>>;

// A ustar archive as tar(1) writes it; type '0' files unless the name ends
// in '/', and an optional override for the type byte.
inline QByteArray makeTar(const Entries& entries, char forcedType = 0)
{
    QByteArray tar;
    for (const auto& [name, data] : entries) {
        char header[512] {};
        std::memcpy(header, name.constData(), std::min<qsizetype>(name.size(), 100));
        std::snprintf(header + 100, 8, "%07o", 0644);
        std::snprintf(header + 124, 12, "%011llo", static_cast<unsigned long long>(data.size()));
        std::memcpy(header + 257, "ustar", 6);
        std::memcpy(header + 263, "00", 2);
        header[156] = forcedType ? forcedType : (name.endsWith('/') ? '5' : '0');
        std::memset(header + 148, ' ', 8);
        unsigned checksum = 0;
        for (unsigned char byte : header)
            checksum += byte;
        std::snprintf(header + 148, 8, "%06o", checksum);
        tar.append(header, 512);
        tar.append(data);
        tar.append(QByteArray((512 - data.size() % 512) % 512, '\0'));
    }
    tar.append(QByteArray(1024, '\0'));
    return tar;
}

// A zstd frame of raw (stored) blocks: magic, a single-segment header with a
// four-byte content size, then blocks of at most 128 KiB.
inline QByteArray makeZstd(const QByteArray& content)
{
    QByteArray frame("\x28\xb5\x2f\xfd", 4);
    frame.append(char(0xa0));
    const quint32 size = static_cast<quint32>(content.size());
    for (int i = 0; i < 4; ++i)
        frame.append(char((size >> (8 * i)) & 0xff));
    qsizetype offset = 0;
    do {
        const qsizetype chunk = std::min<qsizetype>(content.size() - offset, 128 * 1024);
        const bool last = offset + chunk >= content.size();
        const quint32 block = (static_cast<quint32>(chunk) << 3) | (last ? 1u : 0u);
        frame.append(char(block & 0xff)).append(char((block >> 8) & 0xff)).append(char((block >> 16) & 0xff));
        frame.append(content.mid(offset, chunk));
        offset += chunk;
    } while (offset < content.size());
    return frame;
}

// The JS fixture as a package: a login screen, the Selection picker and one
// item action, so registry, hub and screen tests share one provider.
inline Spool::ProviderPackageContents package(
    const QString& id = QStringLiteral("fixture.test"), const QString& version = QStringLiteral("1.0.0"))
{
    const auto read = [](const char *name) {
        QFile file(QStringLiteral(TEST_SOURCE_DIR "/tests/providers/fixtures/") + QLatin1String(name));
        file.open(QIODevice::ReadOnly);
        return file.readAll();
    };
    const QJsonObject manifest { { QStringLiteral("format"), 2 }, { QStringLiteral("api"), QStringLiteral("0.2") },
        { QStringLiteral("id"), id }, { QStringLiteral("name"), QStringLiteral("Fixture") },
        { QStringLiteral("version"), version }, { QStringLiteral("entry"), QStringLiteral("logic/provider.mjs") },
        { QStringLiteral("capabilities"), QJsonArray { QStringLiteral("search") } },
        { QStringLiteral("ui"),
            QJsonObject { { QStringLiteral("login"), QStringLiteral("ui/Login.qml") },
                { QStringLiteral("picker"), QStringLiteral("ui/Selection.qml") } } },
        { QStringLiteral("actions"),
            QJsonArray { QJsonObject { { QStringLiteral("id"), QStringLiteral("tag") },
                { QStringLiteral("label"), QStringLiteral("Tag") },
                { QStringLiteral("types"), QJsonArray { QStringLiteral("Movie") } } } } } };
    const QByteArray json = QJsonDocument(manifest).toJson();
    Spool::ProviderPackageContents contents;
    contents.manifest = *Spool::ProviderManifest::parse(json);
    contents.files
        = { { QStringLiteral("manifest.json"), json }, { QStringLiteral("logic/provider.mjs"), read("provider.mjs") },
              { QStringLiteral("ui/Selection.qml"), read("Selection.qml") },
              { QStringLiteral("ui/Login.qml"), QByteArrayLiteral("import QtQuick\nItem {}\n") } };
    return contents;
}

// The package as the store serves it.
inline QByteArray archive(const Spool::ProviderPackageContents& contents)
{
    Entries entries;
    for (auto it = contents.files.cbegin(); it != contents.files.cend(); ++it)
        entries.push_back({ it.key().toUtf8(), it.value() });
    return makeZstd(makeTar(entries));
}

} // namespace ProviderFixture
