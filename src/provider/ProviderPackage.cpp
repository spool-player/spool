#include "ProviderPackage.h"

#include "../../third_party/zstd/bounded_zstd.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

namespace JellyfinNative {

namespace {
    constexpr qsizetype kMaxArchiveBytes = 16 * 1024 * 1024;
    constexpr qsizetype kMaxExpandedBytes = 32 * 1024 * 1024;
    constexpr qsizetype kMaxFileBytes = 8 * 1024 * 1024;
    constexpr int kMaxFiles = 512;

    bool fail(QString *error, const QString& message)
    {
        if (error)
            *error = message;
        return false;
    }

    bool validPath(const QString& path)
    {
        static const QSet<QString> extensions { QStringLiteral("mjs"), QStringLiteral("js"), QStringLiteral("qml"),
            QStringLiteral("json"), QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("svg"),
            QStringLiteral("webp"), QStringLiteral("ttf"), QStringLiteral("otf"), QStringLiteral("txt"),
            QStringLiteral("md"), QStringLiteral("map") };
        if (path.isEmpty() || path.size() > 200 || path.startsWith(QLatin1Char('/')) || path.contains(QLatin1Char('\\'))
            || path.contains(QLatin1Char(':')))
            return false;
        for (const QString& part : path.split(QLatin1Char('/'))) {
            if (part.isEmpty() || part.startsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' ')))
                return false;
        }
        const QFileInfo info(path);
        return extensions.contains(info.suffix().toLower()) || info.fileName() == QStringLiteral("LICENSE")
            || info.fileName() == QStringLiteral("NOTICE");
    }

    bool nativeBinary(const QByteArray& data)
    {
        return data.startsWith("\x7f"
                               "ELF")
            || data.startsWith("MZ") || data.startsWith(QByteArray("\0asm", 4)) || data.startsWith("\xca\xfe\xba\xbe")
            || data.startsWith("\xce\xfa\xed\xfe") || data.startsWith("\xcf\xfa\xed\xfe");
    }

    std::optional<qint64> octal(const char *field, int length)
    {
        qint64 value = 0;
        int i = 0;
        while (i < length && field[i] == ' ')
            ++i;
        for (; i < length && field[i] >= '0' && field[i] <= '7'; ++i)
            value = value * 8 + (field[i] - '0');
        if (i < length && field[i] != '\0' && field[i] != ' ')
            return std::nullopt;
        return value;
    }

    QString text(const char *field, int length)
    {
        return QString::fromUtf8(field, qstrnlen(field, length));
    }

    bool readTar(const QByteArray& tar, QMap<QString, QByteArray>& files, QString *error)
    {
        qsizetype offset = 0;
        qsizetype expanded = 0;
        QSet<QString> folded;
        while (offset + 512 <= tar.size()) {
            const char *header = tar.constData() + offset;
            if (std::all_of(header, header + 512, [](char c) { return c == 0; }))
                return true;
            qint64 checksum = 0;
            for (int i = 0; i < 512; ++i)
                checksum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(header[i]);
            const auto stored = octal(header + 148, 8);
            const auto size = octal(header + 124, 12);
            if (!stored || *stored != checksum || !size || *size < 0)
                return fail(error, QStringLiteral("Corrupt archive header"));
            const QString prefix = text(header + 345, 155);
            QString name = text(header + 0, 100);
            if (!prefix.isEmpty())
                name = prefix + QLatin1Char('/') + name;
            if (name.startsWith(QStringLiteral("./")))
                name = name.mid(2);
            const char type = header[156];
            offset += 512;
            if (type == '5') {
                if (*size != 0)
                    return fail(error, QStringLiteral("Corrupt directory entry"));
                continue;
            }
            if (type != '0' && type != '\0')
                return fail(error, QStringLiteral("Links and special files are not allowed: %1").arg(name));
            if (!validPath(name))
                return fail(error, QStringLiteral("Forbidden path in package: %1").arg(name));
            if (*size > kMaxFileBytes || (expanded += *size) > kMaxExpandedBytes || files.size() >= kMaxFiles)
                return fail(error, QStringLiteral("Package is too large"));
            if (offset + *size > tar.size())
                return fail(error, QStringLiteral("Truncated archive"));
            if (folded.contains(name.toCaseFolded()))
                return fail(error, QStringLiteral("Duplicate path in package: %1").arg(name));
            folded.insert(name.toCaseFolded());
            QByteArray data = tar.mid(offset, *size);
            if (nativeBinary(data))
                return fail(error, QStringLiteral("Native binaries are not allowed: %1").arg(name));
            files.insert(name, std::move(data));
            offset += (*size + 511) / 512 * 512;
        }
        return fail(error, QStringLiteral("Truncated archive"));
    }

    QString string(const QJsonObject& object, const char *key)
    {
        return object.value(QLatin1String(key)).toString().trimmed();
    }
} // namespace

std::optional<ProviderManifest> ProviderManifest::parse(const QByteArray& json, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    const QJsonObject root = document.object();
    if (!document.isObject()) {
        fail(error, QStringLiteral("manifest.json is not a JSON object"));
        return std::nullopt;
    }
    if (root.value(QStringLiteral("format")).toInt() != 2 || string(root, "api") != QStringLiteral("0.2")) {
        fail(error, QStringLiteral("This provider was built for a different version of Spool"));
        return std::nullopt;
    }
    static const QRegularExpression idPattern(QStringLiteral("^[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+$"));
    static const QRegularExpression versionPattern(QStringLiteral("^\\d+\\.\\d+\\.\\d+(?:-[0-9A-Za-z.]+)?$"));
    ProviderManifest manifest;
    manifest.id = string(root, "id");
    manifest.name = string(root, "name");
    manifest.version = string(root, "version");
    manifest.publisher = string(root, "publisher");
    manifest.summary = string(root, "summary");
    manifest.icon = string(root, "icon");
    manifest.entry = string(root, "entry");
    manifest.homepage = string(root, "homepage");
    for (const QJsonValue& value : root.value(QStringLiteral("capabilities")).toArray())
        manifest.capabilities.append(value.toString());
    for (const QJsonValue& value : root.value(QStringLiteral("origins")).toArray())
        manifest.origins.append(value.toString());
    const QJsonObject ui = root.value(QStringLiteral("ui")).toObject();
    for (auto it = ui.begin(); it != ui.end(); ++it)
        manifest.ui.insert(it.key(), it.value().toString());
    for (const QJsonValue& value : root.value(QStringLiteral("actions")).toArray()) {
        const QJsonObject action = value.toObject();
        if (!action.value(QStringLiteral("id")).toString().isEmpty()
            && !action.value(QStringLiteral("label")).toString().isEmpty())
            manifest.actions.append(action.toVariantMap());
    }

    const bool pathsValid = std::all_of(manifest.ui.cbegin(), manifest.ui.cend(),
        [](const QString& path) { return validPath(path) && path.endsWith(QStringLiteral(".qml")); });
    if (!idPattern.match(manifest.id).hasMatch() || manifest.id.size() > 128 || manifest.name.isEmpty()
        || manifest.name.size() > 64 || manifest.summary.size() > 120
        || !versionPattern.match(manifest.version).hasMatch() || !validPath(manifest.entry)
        || !manifest.entry.endsWith(QStringLiteral(".mjs")) || (!manifest.icon.isEmpty() && !validPath(manifest.icon))
        || !pathsValid) {
        fail(error, QStringLiteral("manifest.json is missing a required field or has an invalid one"));
        return std::nullopt;
    }
    return manifest;
}

namespace ProviderPackage {

    std::optional<ProviderPackageContents> read(const QByteArray& archive, QString *error)
    {
        if (archive.isEmpty() || archive.size() > kMaxArchiveBytes) {
            fail(error, QStringLiteral("Package is empty or too large"));
            return std::nullopt;
        }
        // The expanded tar holds every file plus a header per file and padding.
        const size_t ceiling = kMaxExpandedBytes + (kMaxFiles + 2) * 1024;
        const size_t declared = spool_zstd_content_size(archive.constData(), archive.size());
        QByteArray tar(static_cast<qsizetype>(std::min(declared, ceiling)), Qt::Uninitialized);
        size_t written = 0;
        if (spool_zstd_decompress(archive.constData(), archive.size(), tar.data(), tar.size(), &written) != 0) {
            fail(error, QStringLiteral("Package is not a valid .tar.zst archive"));
            return std::nullopt;
        }
        tar.truncate(static_cast<qsizetype>(written));

        ProviderPackageContents package;
        if (!readTar(tar, package.files, error))
            return std::nullopt;
        const auto manifest = ProviderManifest::parse(package.files.value(QStringLiteral("manifest.json")), error);
        if (!manifest)
            return std::nullopt;
        package.manifest = *manifest;
        QStringList required { manifest->entry };
        required += manifest->ui.values();
        if (!manifest->icon.isEmpty())
            required.append(manifest->icon);
        for (const QString& path : std::as_const(required)) {
            if (!package.files.contains(path)) {
                fail(error, QStringLiteral("manifest.json names a missing file: %1").arg(path));
                return std::nullopt;
            }
        }
        return package;
    }

    std::optional<QString> install(const ProviderPackageContents& package, const QString& root, QString *error)
    {
        const QDir providerDir(QDir(root).filePath(package.manifest.id));
        const QString target = providerDir.filePath(package.manifest.version);
        const QString staging
            = providerDir.filePath(QStringLiteral(".staging-") + QUuid::createUuid().toString(QUuid::Id128));
        const auto abandon = [&](const QString& message) -> std::optional<QString> {
            QDir(staging).removeRecursively();
            fail(error, message);
            return std::nullopt;
        };
        if (!QDir().mkpath(staging))
            return abandon(QStringLiteral("Could not create %1").arg(staging));
        for (auto it = package.files.cbegin(); it != package.files.cend(); ++it) {
            const QString path = QDir(staging).filePath(it.key());
            QDir().mkpath(QFileInfo(path).path());
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(it.value()) != it.value().size() || !file.commit())
                return abandon(QStringLiteral("Could not write %1").arg(it.key()));
        }
        // Versions are immutable: reinstalling the same one replaces it whole.
        QDir(target).removeRecursively();
        if (!QDir().rename(staging, target))
            return abandon(
                QStringLiteral("Could not activate %1 %2").arg(package.manifest.id, package.manifest.version));
        // Only the newest version stays; the running engine has already read
        // its module and components, so an older directory is dead weight.
        for (const QString& other : providerDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (other != package.manifest.version)
                QDir(providerDir.filePath(other)).removeRecursively();
        }
        return target;
    }

    QMap<QString, QString> installedVersions(const QString& root)
    {
        QMap<QString, QString> result;
        const QDir base(root);
        for (const QString& id : base.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QDir providerDir(base.filePath(id));
            QString newest;
            for (const QString& version : providerDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                if (!version.startsWith(QLatin1Char('.')) && (newest.isEmpty() || compareVersions(version, newest) > 0))
                    newest = version;
            }
            if (!newest.isEmpty())
                result.insert(id, providerDir.filePath(newest));
        }
        return result;
    }

    int compareVersions(const QString& left, const QString& right)
    {
        const auto split = [](const QString& version) {
            const qsizetype dash = version.indexOf(QLatin1Char('-'));
            return std::pair { version.left(dash < 0 ? version.size() : dash).split(QLatin1Char('.')),
                dash < 0 ? QString() : version.mid(dash + 1) };
        };
        const auto [leftCore, leftPre] = split(left);
        const auto [rightCore, rightPre] = split(right);
        for (qsizetype i = 0; i < std::max(leftCore.size(), rightCore.size()); ++i) {
            const int a = i < leftCore.size() ? leftCore.at(i).toInt() : 0;
            const int b = i < rightCore.size() ? rightCore.at(i).toInt() : 0;
            if (a != b)
                return a < b ? -1 : 1;
        }
        if (leftPre == rightPre)
            return 0;
        if (leftPre.isEmpty() || rightPre.isEmpty())
            return leftPre.isEmpty() ? 1 : -1;
        return leftPre < rightPre ? -1 : 1;
    }

} // namespace ProviderPackage
} // namespace JellyfinNative
