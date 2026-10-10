#pragma once

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
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

inline bool saveToPath(const QString& credentialPath, const QString& token)
{
    QDir directory = QFileInfo(credentialPath).dir();
    if (!directory.mkpath(QStringLiteral(".")))
        return false;
    if (!QFile::setPermissions(
            directory.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        return false;
    QSaveFile file(credentialPath);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return false;
    const QByteArray bytes = token.toUtf8();
    return file.write(bytes) == bytes.size() && file.commit();
}

inline bool save(const QString& profileId, const QString& token)
{
    return saveToPath(path(profileId), token);
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
