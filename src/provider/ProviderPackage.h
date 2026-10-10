#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace Spool {

// manifest.json, format 3. Everything the host needs to list, install and
// mount a provider without running any of its code.
struct ProviderManifest {
    QString id;
    QString name;
    QString version;
    QString publisher;
    QString summary;
    QString icon;
    QString entry;
    QString homepage;
    QStringList capabilities;
    // Origins every account may reach besides the ones it was configured
    // with; "*" allows any HTTP(S) origin.
    QStringList origins;
    // Role (login, settings, picker, ...) to a QML file inside the package.
    QMap<QString, QString> ui;
    // Item menu entries the provider handles: {id, label, icon, types}.
    // Declared here so opening the menu never waits on the provider.
    QVariantList actions;

    bool needsAccount() const
    {
        return ui.contains(QStringLiteral("login"));
    }
    static std::optional<ProviderManifest> parse(const QByteArray& json, QString *error = nullptr);
};

struct ProviderPackageContents {
    ProviderManifest manifest;
    QMap<QString, QByteArray> files;
};

// A provider ships as one ustar archive compressed with zstd. Reading never
// executes anything: paths, types, sizes and the manifest are validated and
// native binaries are rejected before a byte touches the disk.
namespace ProviderPackage {
    std::optional<ProviderPackageContents> read(const QByteArray& archive, QString *error = nullptr);
    // Staging is inert. The registry admits and activates it only after
    // rechecking its owner, module revision and the caller's consent.
    class StagedPackage {
    public:
        ~StagedPackage();
        StagedPackage(const StagedPackage&) = delete;
        StagedPackage& operator=(const StagedPackage&) = delete;

    private:
        friend std::shared_ptr<StagedPackage> stage(
            const ProviderPackageContents&, const QString&, QString *, const std::function<void(qint64, qint64)>&);
        friend std::optional<QString> activate(StagedPackage&, QString *);
        StagedPackage(QString path, QString target, QString version)
            : m_path(std::move(path))
            , m_target(std::move(target))
            , m_version(std::move(version))
        {
        }
        QString m_path;
        QString m_target;
        QString m_version;
    };
    std::shared_ptr<StagedPackage> stage(const ProviderPackageContents& package, const QString& root,
        QString *error = nullptr, const std::function<void(qint64, qint64)>& progress = {});
    std::optional<QString> activate(StagedPackage& staged, QString *error = nullptr);
    // Writes <root>/<id>/<version>/ through a staging directory so a failed
    // install never leaves a half-written version behind. Returns the
    // version directory.
    std::optional<QString> install(
        const ProviderPackageContents& package, const QString& root, QString *error = nullptr);
    // The newest installed version directory of every provider under root.
    QMap<QString, QString> installedVersions(const QString& root);
    // -1, 0 or 1 comparing dotted numeric versions; a pre-release suffix
    // sorts before its release.
    int compareVersions(const QString& left, const QString& right);
}

} // namespace Spool
