#include "provider/ProviderPackage.h"
#include "ProviderFixture.h"
#include "TestMain.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

using ProviderFixture::Entries;
using ProviderFixture::makeTar;
using ProviderFixture::makeZstd;

QByteArray manifest(const char *id = "spool.test", const char *version = "1.0.0")
{
    return QStringLiteral(R"({"format": 2, "api": "0.2", "id": "%1", "name": "Test", "version": "%2",
        "entry": "logic/provider.mjs", "icon": "assets/icon.svg", "ui": {"login": "ui/Login.qml"},
        "actions": [{"id": "rename", "label": "Rename"}, {"id": "", "label": "dropped"}]})")
        .arg(QLatin1String(id), QLatin1String(version))
        .toUtf8();
}

Entries validEntries(const char *version = "1.0.0")
{
    return { { "manifest.json", manifest("spool.test", version) }, { "logic/", {} },
        { "logic/provider.mjs", "export function createSource() { return {}; }" },
        { "ui/Login.qml", "import QtQuick\nItem {}\n" }, { "assets/icon.svg", "<svg/>" } };
}

bool rejects(const QByteArray& archive, const char *expected)
{
    QString error;
    const auto package = Spool::ProviderPackage::read(archive, &error);
    if (package)
        return false;
    if (!error.contains(QLatin1String(expected))) {
        std::cerr << "unexpected error: " << error.toStdString() << '\n';
        return false;
    }
    return true;
}

} // namespace

SPOOL_TEST_MAIN("provider-package-unpack")
{
    QCoreApplication app(argc, argv);
    using namespace Spool;

    QString error;
    const auto package = ProviderPackage::read(makeZstd(makeTar(validEntries())), &error);
    require(package.has_value(), "a valid package is read");
    require(package->manifest.id == QStringLiteral("spool.test") && package->manifest.version == QStringLiteral("1.0.0")
            && package->manifest.entry == QStringLiteral("logic/provider.mjs"),
        "manifest identity and entry are read");
    require(package->manifest.needsAccount(), "a login screen means the provider needs an account");
    require(package->manifest.actions.size() == 1, "actions without an id are dropped");
    require(package->files.size() == 4 && !package->files.contains(QStringLiteral("logic/")),
        "files are read and directories are not files");

    // Several raw blocks, as a zstd encoder emits for incompressible input.
    Entries large = validEntries();
    QByteArray noise(300 * 1024, '\0');
    for (qsizetype i = 0; i < noise.size(); ++i)
        noise[i] = char((i * 2654435761u) >> 13);
    large.push_back({ "assets/noise.txt", noise });
    const auto multiBlock = ProviderPackage::read(makeZstd(makeTar(large)));
    require(multiBlock && multiBlock->files.value(QStringLiteral("assets/noise.txt")) == noise,
        "multi-block frames decompress exactly");

    // The real bundled package: compressed blocks from the zstd CLI.
    QFile bundled(QStringLiteral(TEST_SOURCE_DIR "/providers/bundled/spool.jellyfin-0.2.0.tar.zst"));
    if (bundled.open(QIODevice::ReadOnly)) {
        const auto jellyfin = ProviderPackage::read(bundled.readAll(), &error);
        require(jellyfin && jellyfin->manifest.id == QStringLiteral("spool.jellyfin"),
            "the bundled Jellyfin package decompresses and validates");
    }

    require(rejects(QByteArrayLiteral("PK\x03\x04not zstd"), "not a valid .tar.zst"), "zip archives are refused");
    QByteArray truncated = makeZstd(makeTar(validEntries()));
    truncated.chop(700);
    require(rejects(truncated, "not a valid .tar.zst"), "truncated frames are refused");

    const auto withEntry = [](QByteArray name, QByteArray data) {
        Entries entries = validEntries();
        entries.push_back({ std::move(name), std::move(data) });
        return makeZstd(makeTar(entries));
    };
    require(rejects(withEntry("../evil.mjs", "x"), "Forbidden path"), "path traversal is refused");
    require(rejects(withEntry("/etc/evil.mjs", "x"), "Forbidden path"), "absolute paths are refused");
    require(rejects(withEntry("logic/.hidden.mjs", "x"), "Forbidden path"), "hidden files are refused");
    require(rejects(withEntry("payload.exe", "x"), "Forbidden path"), "executable extensions are refused");
    require(rejects(withEntry("logic/native.js",
                        QByteArray("\x7f"
                                   "ELF\x02\x01",
                            6)),
                "Native binaries"),
        "native binaries are refused whatever their name");
    require(rejects(withEntry("LOGIC/provider.mjs", "x"), "Duplicate path"), "case-folded duplicates are refused");
    require(rejects(makeZstd(makeTar({ { "manifest.json", manifest() }, { "logic/link.mjs", {} } }, '2')),
                "Links and special files"),
        "symlinks are refused");

    Entries missingUi = validEntries();
    missingUi.erase(missingUi.begin() + 3);
    require(rejects(makeZstd(makeTar(missingUi)), "missing file: ui/Login.qml"), "every named file must exist");
    require(rejects(makeZstd(makeTar({ { "logic/provider.mjs", "x" } })), "not a JSON object"),
        "a package without a manifest is refused");
    Entries oldFormat = validEntries();
    oldFormat.front().second.replace("\"format\": 2", "\"format\": 1");
    require(rejects(makeZstd(makeTar(oldFormat)), "different version of Spool"), "old manifest formats are refused");
    Entries badId = validEntries();
    badId.front().second = manifest("Not An Id");
    require(rejects(makeZstd(makeTar(badId)), "invalid one"), "malformed ids are refused");

    require(ProviderPackage::compareVersions(QStringLiteral("1.10.0"), QStringLiteral("1.9.9")) > 0,
        "versions compare numerically");
    require(ProviderPackage::compareVersions(QStringLiteral("1.0.0-beta"), QStringLiteral("1.0.0")) < 0,
        "a pre-release sorts before its release");
    require(ProviderPackage::compareVersions(QStringLiteral("1.0"), QStringLiteral("1.0.0")) == 0,
        "missing components are zero");

    QTemporaryDir root;
    require(root.isValid(), "temporary directory created");
    const auto installed = ProviderPackage::install(*package, root.path(), &error);
    require(installed && QFile::exists(QDir(*installed).filePath(QStringLiteral("logic/provider.mjs"))),
        "a package installs into its version directory");
    const auto newer = ProviderPackage::read(makeZstd(makeTar(validEntries("1.1.0"))));
    const auto upgraded = ProviderPackage::install(*newer, root.path(), &error);
    require(upgraded && !QFile::exists(*installed), "installing a newer version removes the older one");
    const QStringList leftovers
        = QDir(root.filePath(QStringLiteral("spool.test"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    require(leftovers == QStringList { QStringLiteral("1.1.0") }, "no staging directory is left behind");
    require(ProviderPackage::installedVersions(root.path()).value(QStringLiteral("spool.test")) == *upgraded,
        "the installed version is found again");

    std::cout << "provider package test ok\n";
    return 0;
}
