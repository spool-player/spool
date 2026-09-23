#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <optional>

namespace Spool {

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
};

struct UpdateManifestResult {
    std::optional<UpdateRelease> release;
    QString error;
};

UpdateManifestResult selectUpdate(
    const QByteArray& manifest, int currentVersionCode, bool allowPrerelease, const QString& assetKey);

} // namespace Spool
