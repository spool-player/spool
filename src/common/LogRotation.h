#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

namespace Spool {

// mkdir -p for the directory portion of a log path. The log directory lives
// under /tmp on webOS and is wiped every boot, so every writer must be able
// to recreate it — not just whichever code path happened to run first.
inline void ensureParentDirectoryExists(const char *path)
{
    const QFileInfo info(QString::fromLocal8Bit(path));
    QDir().mkpath(info.absolutePath());
}

inline void makeWebOSSupportLogsReadable(const QString& current)
{
#ifdef SPOOL_WEBOS
    const QFileInfo info(current);
    QFile::setPermissions(info.absolutePath(),
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup
            | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther);

    QFile active(current);
    if (!active.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    active.close();

    constexpr auto permissions
        = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther;
    for (const QString& path : { current, current + QStringLiteral(".1"), current + QStringLiteral(".2") }) {
        if (QFileInfo::exists(path))
            QFile::setPermissions(path, permissions);
    }
#else
    Q_UNUSED(current);
#endif
}

inline void rotateLogFile(const char *path)
{
    ensureParentDirectoryExists(path);
    const QString current = QString::fromLocal8Bit(path);
    const QString first = current + QStringLiteral(".1");
    const QString second = current + QStringLiteral(".2");
    QFile::remove(second);
    QFile::rename(first, second);
    QFile::rename(current, first);
    makeWebOSSupportLogsReadable(current);
}

} // namespace Spool
