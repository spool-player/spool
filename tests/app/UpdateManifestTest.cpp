#include "app/UpdateManifest.h"

#include "TestMain.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdlib>
#include <iostream>

using namespace JellyfinNative;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

QJsonObject release(const QString& channel, int versionCode, const QString& version, const QString& assetKey)
{
    const QString tag = QStringLiteral("v") + version;
    const QString packageName = assetKey == QStringLiteral("arm")
        ? QStringLiteral("com.sachk.spool_") + version + QStringLiteral("_arm.ipk")
        : QStringLiteral("spool-") + assetKey + QStringLiteral(".apk");
    return {
        { QStringLiteral("channel"), channel },
        { QStringLiteral("version"), version },
        { QStringLiteral("versionCode"), versionCode },
        { QStringLiteral("notes"), QStringLiteral("Security fixes first.\n\nMajor features next.") },
        { QStringLiteral("releaseUrl"), QStringLiteral("https://github.com/spool-player/spool/releases/tag/") + tag },
        { QStringLiteral("assets"),
            QJsonObject {
                { assetKey,
                    QJsonObject {
                        { QStringLiteral("url"),
                            QStringLiteral("https://github.com/spool-player/spool/releases/download/") + tag
                                + QLatin1Char('/') + packageName },
                        { QStringLiteral("sha256"), QString(64, QLatin1Char('a')) },
                        { QStringLiteral("size"), 12000000 },
                    } },
            } },
    };
}

QByteArray manifest(std::initializer_list<QJsonObject> releases)
{
    QJsonArray array;
    for (const QJsonObject& item : releases)
        array.append(item);
    return QJsonDocument(QJsonObject {
                             { QStringLiteral("schemaVersion"), 1 },
                             { QStringLiteral("releases"), array },
                         })
        .toJson(QJsonDocument::Compact);
}

void stableChannelExcludesPrereleases(const QString& assetKey)
{
    const UpdateManifestResult result
        = selectUpdate(manifest({ release(QStringLiteral("release"), 700099, QStringLiteral("0.7.0"), assetKey),
                           release(QStringLiteral("prerelease"), 800001, QStringLiteral("0.8.0-beta.1"), assetKey) }),
            600099, false, assetKey);
    require(result.error.isEmpty(), "valid stable manifest was rejected");
    require(result.release.has_value(), "stable update was not selected");
    require(result.release->versionCode == 700099, "stable channel selected a prerelease");
}

void prereleaseChannelSelectsLargestEligibleBuild(const QString& assetKey)
{
    UpdateManifestResult result = selectUpdate(
        manifest({ release(QStringLiteral("prerelease"), 800001, QStringLiteral("0.8.0-beta.1"), assetKey),
            release(QStringLiteral("release"), 700099, QStringLiteral("0.7.0"), assetKey) }),
        600099, true, assetKey);
    require(result.error.isEmpty() && result.release && result.release->versionCode == 800001,
        "prerelease channel did not choose the largest eligible build");

    result = selectUpdate(
        manifest({ release(QStringLiteral("prerelease"), 800001, QStringLiteral("0.8.0-beta.1"), assetKey),
            release(QStringLiteral("release"), 800099, QStringLiteral("0.8.0"), assetKey) }),
        800001, true, assetKey);
    require(result.error.isEmpty() && result.release && result.release->versionCode == 800099,
        "prerelease channel did not upgrade to a larger release build");
}

void currentOrOlderBuildsAreIgnored(const QString& assetKey)
{
    const QByteArray releases
        = manifest({ release(QStringLiteral("release"), 700099, QStringLiteral("0.7.0"), assetKey),
            release(QStringLiteral("prerelease"), 700001, QStringLiteral("0.7.0-beta.1"), assetKey),
            release(QStringLiteral("release"), 600099, QStringLiteral("0.6.0"), assetKey) });
    for (const bool allowPrerelease : { false, true }) {
        const UpdateManifestResult result = selectUpdate(releases, 700099, allowPrerelease, assetKey);
        require(result.error.isEmpty(), "current or older release produced a manifest error");
        require(!result.release, "current or older release was offered as an update");
    }
}

void missingDeviceAssetIsRejected(const QString& assetKey)
{
    const QJsonObject item
        = release(QStringLiteral("release"), 700099, QStringLiteral("0.7.0"), QStringLiteral("universal"));
    const auto result = selectUpdate(manifest({ item }), 600099, false, assetKey);
    require(!result.release && !result.error.isEmpty(), "missing device package fell back to the universal package");
}

void untrustedUrlsAreRejected(const QString& assetKey)
{
    const QString trustedAsset
        = QStringLiteral("https://github.com/spool-player/spool/releases/download/v0.7.0/package");
    for (const QString& url : {
             QStringLiteral("https://example.com/package"),
             QStringLiteral("https://github.com/other/spool/releases/download/v0.7.0/package"),
             QStringLiteral("http://github.com/spool-player/spool/releases/download/v0.7.0/package"),
             QStringLiteral("https://user@github.com/spool-player/spool/releases/download/v0.7.0/package"),
             QStringLiteral("https://github.com:444/spool-player/spool/releases/download/v0.7.0/package"),
             trustedAsset + QStringLiteral("?download=1"),
             trustedAsset + QStringLiteral("#fragment"),
         }) {
        QJsonObject item = release(QStringLiteral("release"), 700099, QStringLiteral("0.7.0"), assetKey);
        QJsonObject assets = item.value(QStringLiteral("assets")).toObject();
        QJsonObject package = assets.value(assetKey).toObject();
        package.insert(QStringLiteral("url"), url);
        assets.insert(assetKey, package);
        item.insert(QStringLiteral("assets"), assets);
        const auto result = selectUpdate(manifest({ item }), 600099, false, assetKey);
        require(!result.release && !result.error.isEmpty(), "untrusted package URL was accepted");
    }

    QJsonObject item = release(QStringLiteral("release"), 700099, QStringLiteral("0.7.0"), assetKey);
    item.insert(QStringLiteral("releaseUrl"), QStringLiteral("https://github.com/other/spool/releases/tag/v0.7.0"));
    const auto result = selectUpdate(manifest({ item }), 600099, false, assetKey);
    require(!result.release && !result.error.isEmpty(), "off-repository release URL was accepted");
}

void incompleteAssetsAreRejected(const QString& assetKey)
{
    const QJsonObject valid = release(QStringLiteral("release"), 700099, QStringLiteral("0.7.0"), assetKey);
    const QJsonObject validPackage = valid.value(QStringLiteral("assets")).toObject().value(assetKey).toObject();
    QJsonObject missingHash = validPackage;
    missingHash.remove(QStringLiteral("sha256"));
    QJsonObject shortHash = validPackage;
    shortHash.insert(QStringLiteral("sha256"), QStringLiteral("abcd"));
    QJsonObject nonHexHash = validPackage;
    nonHexHash.insert(QStringLiteral("sha256"), QString(64, QLatin1Char('g')));
    QJsonObject emptyPackage = validPackage;
    emptyPackage.insert(QStringLiteral("size"), 0);
    for (const QJsonObject& package : { missingHash, shortHash, nonHexHash, emptyPackage }) {
        QJsonObject item = valid;
        item.insert(QStringLiteral("assets"), QJsonObject { { assetKey, package } });
        const auto result = selectUpdate(manifest({ item }), 600099, false, assetKey);
        require(!result.release && !result.error.isEmpty(), "incomplete package details were accepted");
    }
}

void webOSSelectsArmPackage()
{
    QJsonObject item = release(QStringLiteral("release"), 701399, QStringLiteral("0.7.13"), QStringLiteral("arm"));
    QJsonObject assets = item.value(QStringLiteral("assets")).toObject();
    assets.insert(QStringLiteral("arm64-v8a"), QJsonObject {});
    item.insert(QStringLiteral("assets"), assets);
    const auto result = selectUpdate(manifest({ item }), 701299, false, QStringLiteral("arm"));
    require(result.error.isEmpty() && result.release, "webOS ARM update was not selected");
    require(result.release->packageUrl
            == QUrl(QStringLiteral(
                "https://github.com/spool-player/spool/releases/download/v0.7.13/com.sachk.spool_0.7.13_arm.ipk")),
        "webOS selected another platform's package");
}

} // namespace

JELLYFIN_TEST_MAIN("update-manifest")
{
    QCoreApplication app(argc, argv);
    const QString assetKey = QStringLiteral("arm64-v8a");
    stableChannelExcludesPrereleases(assetKey);
    prereleaseChannelSelectsLargestEligibleBuild(assetKey);
    currentOrOlderBuildsAreIgnored(assetKey);
    missingDeviceAssetIsRejected(assetKey);
    untrustedUrlsAreRejected(assetKey);
    incompleteAssetsAreRejected(assetKey);
    webOSSelectsArmPackage();
    return EXIT_SUCCESS;
}
