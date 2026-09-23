#pragma once

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

namespace Spool::CredentialStore::FileBackend {

inline QString root()
{
    return qEnvironmentVariable("SPOOL_CREDENTIAL_STORE_DIR");
}

inline bool enabled()
{
    return !root().isEmpty();
}

inline QString path(const QString& profileId)
{
    const QString name
        = QString::fromLatin1(QCryptographicHash::hash(profileId.toUtf8(), QCryptographicHash::Sha256).toHex());
    return QDir(root()).filePath(name);
}

inline QString load(const QString& profileId)
{
    QFile file(path(profileId));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

inline bool save(const QString& profileId, const QString& token)
{
    const QString credentialPath = path(profileId);
    QDir directory = QFileInfo(credentialPath).dir();
    if (!directory.mkpath(QStringLiteral(".")))
        return false;
    QFile::setPermissions(directory.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QFile file(credentialPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return file.write(token.toUtf8()) >= 0;
}

inline void remove(const QString& profileId)
{
    QFile::remove(path(profileId));
}

inline void clear()
{
    QDir(root()).removeRecursively();
}

} // namespace Spool::CredentialStore::FileBackend
