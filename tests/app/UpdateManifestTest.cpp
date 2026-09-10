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

QJsonObject release(const QString& channel, int versionCode, const QString& version)
{
    const QString tag = QStringLiteral("v") + version;
    return {
        { QStringLiteral("channel"), channel },
        { QStringLiteral("version"), version },
        { QStringLiteral("versionCode"), versionCode },
        { QStringLiteral("notes"), QStringLiteral("Security fixes first.\n\nMajor features next.") },
        { QStringLiteral("releaseUrl"), QStringLiteral("https://github.com/sachk/spool/releases/tag/") + tag },
        { QStringLiteral("assets"),
            QJsonObject {
                { QStringLiteral("arm64-v8a"),
                    QJsonObject {
                        { QStringLiteral("url"),
                            QStringLiteral("https://github.com/sachk/spool/releases/download/") + tag
                                + QStringLiteral("/spool-arm64-v8a.apk") },
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

void stableChannelExcludesPrereleases()
{
    const UpdateManifestResult result
        = selectAndroidUpdate(manifest({ release(QStringLiteral("release"), 700099, QStringLiteral("0.7.0")),
                                  release(QStringLiteral("prerelease"), 800001, QStringLiteral("0.8.0-beta.1")) }),
            600099, false, QStringLiteral("arm64-v8a"));
    require(result.error.isEmpty(), "valid stable manifest was rejected");
    require(result.release.has_value(), "stable update was not selected");
    require(result.release->versionCode == 700099, "stable channel selected a prerelease");
}

void prereleaseChannelSelectsLargestEligibleBuild()
{
    UpdateManifestResult result
        = selectAndroidUpdate(manifest({ release(QStringLiteral("prerelease"), 800001, QStringLiteral("0.8.0-beta.1")),
                                  release(QStringLiteral("release"), 700099, QStringLiteral("0.7.0")) }),
            600099, true, QStringLiteral("arm64-v8a"));
    require(result.release && result.release->versionCode == 800001,
        "prerelease channel did not choose the largest eligible build");

    result
        = selectAndroidUpdate(manifest({ release(QStringLiteral("prerelease"), 800001, QStringLiteral("0.8.0-beta.1")),
                                  release(QStringLiteral("release"), 800099, QStringLiteral("0.8.0")) }),
            800001, true, QStringLiteral("arm64-v8a"));
    require(result.release && result.release->versionCode == 800099,
        "prerelease channel did not upgrade to a larger release build");
}

void currentOrOlderBuildsAreIgnored()
{
    const UpdateManifestResult result
        = selectAndroidUpdate(manifest({ release(QStringLiteral("release"), 700099, QStringLiteral("0.7.0")) }), 700099,
            false, QStringLiteral("arm64-v8a"));
    require(result.error.isEmpty(), "current release produced a manifest error");
    require(!result.release, "current release was offered as an update");
}

void untrustedOrIncompleteAssetsAreRejected()
{
    QJsonObject item = release(QStringLiteral("release"), 700099, QStringLiteral("0.7.0"));
    QJsonObject assets = item.value(QStringLiteral("assets")).toObject();
    QJsonObject apk = assets.value(QStringLiteral("arm64-v8a")).toObject();
    apk.insert(QStringLiteral("url"), QStringLiteral("https://example.com/spool.apk"));
    assets.insert(QStringLiteral("arm64-v8a"), apk);
    item.insert(QStringLiteral("assets"), assets);
    require(!selectAndroidUpdate(manifest({ item }), 600099, false, QStringLiteral("arm64-v8a")).error.isEmpty(),
        "off-repository APK URL was accepted");

    item = release(QStringLiteral("release"), 700099, QStringLiteral("0.7.0"));
    assets = item.value(QStringLiteral("assets")).toObject();
    apk = assets.value(QStringLiteral("arm64-v8a")).toObject();
    apk.insert(QStringLiteral("sha256"), QStringLiteral("abcd"));
    assets.insert(QStringLiteral("arm64-v8a"), apk);
    item.insert(QStringLiteral("assets"), assets);
    require(!selectAndroidUpdate(manifest({ item }), 600099, false, QStringLiteral("arm64-v8a")).error.isEmpty(),
        "short SHA-256 was accepted");
}

QJsonObject webOSRelease(const QString& version, bool withDigest = true)
{
    const QString tag = QStringLiteral("v") + version;
    const QString base = QStringLiteral("https://github.com/sachk/spool/releases/download/") + tag + QLatin1Char('/');
    return {
        { QStringLiteral("tag_name"), tag },
        { QStringLiteral("draft"), false },
        { QStringLiteral("prerelease"), true },
        { QStringLiteral("published_at"), QStringLiteral("2026-09-04T19:07:35Z") },
        { QStringLiteral("html_url"), QStringLiteral("https://github.com/sachk/spool/releases/tag/") + tag },
        { QStringLiteral("assets"),
            QJsonArray {
                QJsonObject {
                    { QStringLiteral("name"),
                        QStringLiteral("com.sachk.spool_") + version + QStringLiteral("_arm.ipk") },
                    { QStringLiteral("browser_download_url"),
                        base + QStringLiteral("com.sachk.spool_") + version + QStringLiteral("_arm.ipk") },
                    { QStringLiteral("state"), QStringLiteral("uploaded") },
                    { QStringLiteral("size"), 12345 },
                    { QStringLiteral("digest"),
                        withDigest ? QStringLiteral("sha256:") + QString(64, QLatin1Char('a')) : QString() },
                },
                QJsonObject {
                    { QStringLiteral("name"), QStringLiteral("SHA256SUMS.txt") },
                    { QStringLiteral("browser_download_url"), base + QStringLiteral("SHA256SUMS.txt") },
                    { QStringLiteral("state"), QStringLiteral("uploaded") },
                    { QStringLiteral("size"), 512 },
                },
            } },
    };
}

void webOSSelectsSemanticVersionIncludingPrereleases()
{
    QJsonObject draft = webOSRelease(QStringLiteral("9.0.0"));
    draft.insert(QStringLiteral("draft"), true);
    const auto result = selectWebOSUpdate(QJsonDocument(QJsonArray {
                                                            webOSRelease(QStringLiteral("0.9.0")),
                                                            webOSRelease(QStringLiteral("0.10.0-beta.2")),
                                                            draft,
                                                            webOSRelease(QStringLiteral("0.10.0-beta.10")),
                                                        })
            .toJson());
    require(result.error.isEmpty() && result.release && result.release->version == QStringLiteral("0.10.0-beta.10"),
        "webOS did not select highest published semantic prerelease");
    const auto stable = selectWebOSUpdate(QJsonDocument(QJsonArray {
                                                            webOSRelease(QStringLiteral("0.10.0")),
                                                            webOSRelease(QStringLiteral("0.10.0-rc.99")),
                                                        })
            .toJson());
    require(stable.release && stable.release->version == QStringLiteral("0.10.0"),
        "webOS ranked prerelease above final version");
}

void webOSRejectsUntrustedMetadata()
{
    QJsonObject item = webOSRelease(QStringLiteral("0.7.13"));
    QJsonArray assets = item.value(QStringLiteral("assets")).toArray();
    QJsonObject ipk = assets[0].toObject();
    ipk.insert(
        QStringLiteral("browser_download_url"), QStringLiteral("https://example.com/com.sachk.spool_0.7.13_arm.ipk"));
    assets[0] = ipk;
    item.insert(QStringLiteral("assets"), assets);
    require(!selectWebOSUpdate(QJsonDocument(QJsonArray { item }).toJson()).error.isEmpty(),
        "webOS accepted an off-repository IPK");
    item = webOSRelease(QStringLiteral("0.7.13"), false);
    assets = item.value(QStringLiteral("assets")).toArray();
    assets.removeLast();
    item.insert(QStringLiteral("assets"), assets);
    const auto unhashed = selectWebOSUpdate(QJsonDocument(QJsonArray { item }).toJson());
    require(!unhashed.release, "webOS offered an IPK without a trusted hash source");
}

void webOSChecksumsBindExactAsset()
{
    const auto result = selectWebOSUpdate(QJsonDocument(QJsonArray {
                                                            webOSRelease(QStringLiteral("0.7.13"), false),
                                                        })
            .toJson());
    require(result.release.has_value(), "webOS rejected a release with checksum metadata");
    const QByteArray name = result.release->packageName.toUtf8();
    const QByteArray hash(64, 'b');
    require(webOSPackageSha256(hash + "  " + name + "\n", result.release->packageName) == hash,
        "webOS did not accept the exact SHA256SUMS entry");
    require(webOSPackageSha256(hash + "  other_arm.ipk\n", result.release->packageName).isEmpty(),
        "webOS accepted another package's checksum");
    require(webOSPackageSha256(
                hash + "  " + name + "\n" + QByteArray(64, 'c') + "  " + name + "\n", result.release->packageName)
                .isEmpty(),
        "webOS accepted conflicting duplicate checksums");
}

} // namespace

JELLYFIN_TEST_MAIN("update-manifest")
{
    QCoreApplication app(argc, argv);
    stableChannelExcludesPrereleases();
    prereleaseChannelSelectsLargestEligibleBuild();
    currentOrOlderBuildsAreIgnored();
    untrustedOrIncompleteAssetsAreRejected();
    webOSSelectsSemanticVersionIncludingPrereleases();
    webOSRejectsUntrustedMetadata();
    webOSChecksumsBindExactAsset();
    return EXIT_SUCCESS;
}
