#include "UpdateManifest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace JellyfinNative {

namespace {

    constexpr auto kReleaseHost = "github.com";
    constexpr auto kReleaseAssetPrefix = "/spool-player/spool/releases/download/";

    bool isHexSha256(const QString& value)
    {
        if (value.size() != 64)
            return false;
        for (const QChar character : value) {
            const ushort code = character.unicode();
            if (!((code >= '0' && code <= '9') || (code >= 'a' && code <= 'f') || (code >= 'A' && code <= 'F')))
                return false;
        }
        return true;
    }

    bool isExpectedReleaseUrl(const QUrl& url, bool asset)
    {
        if (!url.isValid() || url.scheme() != QStringLiteral("https")
            || url.host().compare(QString::fromLatin1(kReleaseHost), Qt::CaseInsensitive) != 0
            || !url.userInfo().isEmpty() || (url.port() != -1 && url.port() != 443) || url.hasQuery()
            || url.hasFragment()) {
            return false;
        }
        const QString expectedPath
            = asset ? QString::fromLatin1(kReleaseAssetPrefix) : QStringLiteral("/spool-player/spool/releases/tag/");
        return url.path().startsWith(expectedPath);
    }

    std::optional<UpdateChannel> parseChannel(const QString& value)
    {
        if (value == QStringLiteral("release"))
            return UpdateChannel::Release;
        if (value == QStringLiteral("prerelease"))
            return UpdateChannel::Prerelease;
        return std::nullopt;
    }

}

UpdateManifestResult selectUpdate(
    const QByteArray& manifest, int currentVersionCode, bool allowPrerelease, const QString& assetKey)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(manifest, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return { {}, QStringLiteral("The update information is not valid JSON.") };

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schemaVersion")).toInt() != 1)
        return { {}, QStringLiteral("The update information uses an unsupported format.") };

    const QJsonValue releasesValue = root.value(QStringLiteral("releases"));
    if (!releasesValue.isArray())
        return { {}, QStringLiteral("The update information has no release list.") };

    std::optional<UpdateRelease> selected;
    for (const QJsonValue& value : releasesValue.toArray()) {
        if (!value.isObject())
            return { {}, QStringLiteral("The update information contains an invalid release.") };
        const QJsonObject object = value.toObject();
        const auto channel = parseChannel(object.value(QStringLiteral("channel")).toString());
        if (!channel)
            return { {}, QStringLiteral("The update information contains an unknown channel.") };
        if (*channel == UpdateChannel::Prerelease && !allowPrerelease)
            continue;

        const int versionCode = object.value(QStringLiteral("versionCode")).toInt();
        if (versionCode <= currentVersionCode)
            continue;
        const QString version = object.value(QStringLiteral("version")).toString().trimmed();
        const QUrl releaseUrl(object.value(QStringLiteral("releaseUrl")).toString());
        const QJsonValue assetsValue = object.value(QStringLiteral("assets"));
        if (version.isEmpty() || versionCode <= 0 || !isExpectedReleaseUrl(releaseUrl, false)
            || !assetsValue.isObject())
            return { {}, QStringLiteral("The update information contains incomplete release details.") };

        const QJsonValue assetValue = assetsValue.toObject().value(assetKey);
        if (!assetValue.isObject())
            return { {}, QStringLiteral("This update has no package for this device.") };
        const QJsonObject asset = assetValue.toObject();
        const QUrl packageUrl(asset.value(QStringLiteral("url")).toString());
        const QString sha256 = asset.value(QStringLiteral("sha256")).toString();
        const qint64 size = asset.value(QStringLiteral("size")).toInteger();
        if (!isExpectedReleaseUrl(packageUrl, true) || !isHexSha256(sha256) || size <= 0)
            return { {}, QStringLiteral("The update package details are invalid.") };

        UpdateRelease candidate {
            .channel = *channel,
            .version = version,
            .versionCode = versionCode,
            .notes = object.value(QStringLiteral("notes")).toString(),
            .releaseUrl = releaseUrl,
            .packageUrl = packageUrl,
            .packageSha256 = sha256.toLatin1().toLower(),
            .packageSize = size,
        };
        if (!selected || candidate.versionCode > selected->versionCode)
            selected = std::move(candidate);
    }

    return { std::move(selected), {} };
}

} // namespace JellyfinNative
