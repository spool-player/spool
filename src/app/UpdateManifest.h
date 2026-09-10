#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <optional>

namespace JellyfinNative {

enum class UpdateChannel {
    Release,
    Prerelease,
};

struct UpdateRelease {
    UpdateChannel channel = UpdateChannel::Release;
    QString version;
    int versionCode = 0;
    QString notes;
    QUrl releaseUrl;
    QUrl packageUrl;
    QByteArray packageSha256;
    qint64 packageSize = 0;
    QString packageName {};
    QUrl checksumsUrl {};
};

struct UpdateManifestResult {
    std::optional<UpdateRelease> release;
    QString error;
};

UpdateManifestResult selectAndroidUpdate(
    const QByteArray& manifest, int currentVersionCode, bool allowPrerelease, const QString& assetKey);

// GitHub's published releases include prereleases. No installed-version
// comparison is made: the webOS experiment offers this package every launch.
UpdateManifestResult selectWebOSUpdate(const QByteArray& releases);
QByteArray webOSPackageSha256(const QByteArray& checksums, const QString& packageName);

} // namespace JellyfinNative
