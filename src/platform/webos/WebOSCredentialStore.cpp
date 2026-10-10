#include "../CredentialStore.h"
#include "../common/CredentialStoreFileBackend.h"

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
    return FileBackend::saveToPath(credentialPath(profileId), accessToken);
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
