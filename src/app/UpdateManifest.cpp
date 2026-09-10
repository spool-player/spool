#include "UpdateManifest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

namespace JellyfinNative {

namespace {

    constexpr auto kReleaseHost = "github.com";
    constexpr auto kReleaseAssetPrefix = "/sachk/spool/releases/download/";

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
            = asset ? QString::fromLatin1(kReleaseAssetPrefix) : QStringLiteral("/sachk/spool/releases/tag/");
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

    struct SemanticVersion {
        QStringList core;
        QStringList prerelease;
    };

    std::optional<SemanticVersion> semanticVersion(const QString& tag)
    {
        static const QRegularExpression expression(
            QStringLiteral("^v?(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)"
                           "(?:-([0-9A-Za-z-]+(?:\\.[0-9A-Za-z-]+)*))?"
                           "(?:\\+[0-9A-Za-z-]+(?:\\.[0-9A-Za-z-]+)*)?$"));
        const auto match = expression.match(tag);
        if (!match.hasMatch())
            return {};
        SemanticVersion version { { match.captured(1), match.captured(2), match.captured(3) }, {} };
        if (!match.captured(4).isEmpty())
            version.prerelease = match.captured(4).split(QLatin1Char('.'));
        for (const QString& part : version.prerelease) {
            bool numeric = true;
            for (const QChar character : part)
                numeric &= character >= QLatin1Char('0') && character <= QLatin1Char('9');
            if (numeric && part.size() > 1 && part.startsWith(QLatin1Char('0')))
                return {};
        }
        return version;
    }

    int compareNumeric(const QString& left, const QString& right)
    {
        if (left.size() != right.size())
            return left.size() < right.size() ? -1 : 1;
        return QString::compare(left, right, Qt::CaseSensitive);
    }

    int compareVersion(const SemanticVersion& left, const SemanticVersion& right)
    {
        for (qsizetype index = 0; index < 3; ++index) {
            const int comparison = compareNumeric(left.core[index], right.core[index]);
            if (comparison != 0)
                return comparison;
        }
        if (left.prerelease.isEmpty() != right.prerelease.isEmpty())
            return left.prerelease.isEmpty() ? 1 : -1;
        for (qsizetype index = 0; index < qMin(left.prerelease.size(), right.prerelease.size()); ++index) {
            const QString& a = left.prerelease[index];
            const QString& b = right.prerelease[index];
            const auto numeric = [](const QString& value) {
                for (const QChar character : value) {
                    if (character < QLatin1Char('0') || character > QLatin1Char('9'))
                        return false;
                }
                return true;
            };
            const bool aNumeric = numeric(a);
            const bool bNumeric = numeric(b);
            const int comparison = aNumeric != bNumeric ? (aNumeric ? -1 : 1)
                : aNumeric                              ? compareNumeric(a, b)
                                                        : QString::compare(a, b, Qt::CaseSensitive);
            if (comparison != 0)
                return comparison;
        }
        return left.prerelease.size() == right.prerelease.size() ? 0
            : left.prerelease.size() < right.prerelease.size()   ? -1
                                                                 : 1;
    }

}

UpdateManifestResult selectAndroidUpdate(
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
            return { {}, QStringLiteral("This update has no APK for this device.") };
        const QJsonObject asset = assetValue.toObject();
        const QUrl apkUrl(asset.value(QStringLiteral("url")).toString());
        const QString sha256 = asset.value(QStringLiteral("sha256")).toString();
        const qint64 size = asset.value(QStringLiteral("size")).toInteger();
        if (!isExpectedReleaseUrl(apkUrl, true) || !isHexSha256(sha256) || size <= 0)
            return { {}, QStringLiteral("The update APK details are invalid.") };

        UpdateRelease candidate {
            .channel = *channel,
            .version = version,
            .versionCode = versionCode,
            .notes = object.value(QStringLiteral("notes")).toString(),
            .releaseUrl = releaseUrl,
            .packageUrl = apkUrl,
            .packageSha256 = sha256.toLatin1().toLower(),
            .packageSize = size,
        };
        if (!selected || candidate.versionCode > selected->versionCode)
            selected = std::move(candidate);
    }

    return { std::move(selected), {} };
}

UpdateManifestResult selectWebOSUpdate(const QByteArray& releases)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(releases, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray())
        return { {}, QStringLiteral("GitHub returned invalid release information.") };

    std::optional<UpdateRelease> selected;
    std::optional<SemanticVersion> selectedVersion;
    for (const QJsonValue& value : document.array()) {
        if (!value.isObject())
            return { {}, QStringLiteral("GitHub returned an invalid release.") };
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("draft")).toBool(true)
            || object.value(QStringLiteral("published_at")).toString().isEmpty())
            continue;
        const QString tag = object.value(QStringLiteral("tag_name")).toString();
        const auto version = semanticVersion(tag);
        if (!version)
            continue;
        const QUrl releaseUrl(object.value(QStringLiteral("html_url")).toString());
        if (!isExpectedReleaseUrl(releaseUrl, false)
            || releaseUrl.path() != QStringLiteral("/sachk/spool/releases/tag/") + tag)
            return { {}, QStringLiteral("The release URL is not from the Spool repository.") };

        UpdateRelease candidate;
        candidate.channel
            = object.value(QStringLiteral("prerelease")).toBool() ? UpdateChannel::Prerelease : UpdateChannel::Release;
        candidate.version = tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag;
        candidate.releaseUrl = releaseUrl;
        candidate.notes = object.value(QStringLiteral("body")).toString();
        const QJsonValue assetsValue = object.value(QStringLiteral("assets"));
        if (!assetsValue.isArray())
            return { {}, QStringLiteral("The release has no valid asset list.") };
        for (const QJsonValue& assetValue : assetsValue.toArray()) {
            const QJsonObject asset = assetValue.toObject();
            const QString name = asset.value(QStringLiteral("name")).toString();
            const bool package
                = name.startsWith(QStringLiteral("com.sachk.spool_")) && name.endsWith(QStringLiteral("_arm.ipk"));
            const bool checksums = name == QStringLiteral("SHA256SUMS.txt");
            if ((!package && !checksums)
                || asset.value(QStringLiteral("state")).toString() != QStringLiteral("uploaded"))
                continue;
            const QUrl url(asset.value(QStringLiteral("browser_download_url")).toString());
            const qint64 size = asset.value(QStringLiteral("size")).toInteger();
            if (!isExpectedReleaseUrl(url, true) || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
                || url.path() != QString::fromLatin1(kReleaseAssetPrefix) + tag + QLatin1Char('/') + name || size <= 0)
                return { {}, QStringLiteral("The release contains an untrusted package or checksum URL.") };
            if (checksums) {
                if (!candidate.checksumsUrl.isEmpty() || size > 512 * 1024)
                    return { {}, QStringLiteral("The release checksum file is invalid.") };
                candidate.checksumsUrl = url;
                continue;
            }
            if (!candidate.packageUrl.isEmpty())
                return { {}, QStringLiteral("The release contains more than one Spool ARM package.") };
            candidate.packageUrl = url;
            candidate.packageName = name;
            candidate.packageSize = size;
            const QString digest = asset.value(QStringLiteral("digest")).toString();
            if (!digest.isEmpty()) {
                if (!digest.startsWith(QStringLiteral("sha256:")) || !isHexSha256(digest.mid(7)))
                    return { {}, QStringLiteral("The package SHA-256 digest is invalid.") };
                candidate.packageSha256 = digest.mid(7).toLatin1().toLower();
            }
        }
        if (candidate.packageUrl.isEmpty() || (candidate.packageSha256.isEmpty() && candidate.checksumsUrl.isEmpty()))
            continue;
        if (!selectedVersion || compareVersion(*version, *selectedVersion) > 0) {
            selected = std::move(candidate);
            selectedVersion = version;
        }
    }
    return { std::move(selected), {} };
}

QByteArray webOSPackageSha256(const QByteArray& checksums, const QString& packageName)
{
    QByteArray selected;
    const QByteArray expectedName = packageName.toUtf8();
    for (QByteArray line : checksums.split('\n')) {
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.size() < 67 || line[64] != ' ' || (line[65] != ' ' && line[65] != '*'))
            continue;
        if (line.mid(66) != expectedName)
            continue;
        const QByteArray hash = line.first(64).toLower();
        if (!isHexSha256(QString::fromLatin1(hash)) || !selected.isEmpty())
            return {};
        selected = hash;
    }
    return selected;
}

} // namespace JellyfinNative
