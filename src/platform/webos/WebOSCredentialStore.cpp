#include "../CredentialStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace Spool::CredentialStore {
namespace {

    QString credentialRoot()
    {
        return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
            .filePath(QStringLiteral("credentials"));
    }

    QString credentialPath(const QString& profileId)
    {
        const QString name
            = QString::fromLatin1(QCryptographicHash::hash(profileId.toUtf8(), QCryptographicHash::Sha256).toHex());
        return QDir(credentialRoot()).filePath(name);
    }

} // namespace

QString load(const QString& profileId)
{
    QFile file(credentialPath(profileId));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

bool save(const QString& profileId, const QString& accessToken)
{
    const QString root = credentialRoot();
    if (!QDir().mkpath(root))
        return false;
    QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QFile file(credentialPath(profileId));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return file.write(accessToken.toUtf8()) >= 0;
}

void remove(const QString& profileId)
{
    QFile::remove(credentialPath(profileId));
}

void clear()
{
    QDir(credentialRoot()).removeRecursively();
}

} // namespace Spool::CredentialStore
