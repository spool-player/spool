#include "platform/MpvConfigPolicy.h"

#include <QDir>
#include <QFileInfo>
#include <QUrl>

namespace Spool {

MpvConfigPolicy validatedPlatformMpvConfigPolicy(const QString& mode, const QString& directory)
{
    if (mode.compare(QStringLiteral("standard"), Qt::CaseInsensitive) == 0)
        return { MpvConfigPolicy::Mode::Standard, {}, true, {} };
    if (mode.compare(QStringLiteral("custom"), Qt::CaseInsensitive) != 0)
        return {};

    const QUrl selectedUrl(directory.trimmed());
    const QString selectedPath = selectedUrl.isLocalFile() ? selectedUrl.toLocalFile() : directory.trimmed();
    const QString cleaned = QDir::cleanPath(selectedPath);
    if (cleaned.isEmpty() || !QDir::isAbsolutePath(cleaned)) {
        return { MpvConfigPolicy::Mode::Disabled, {}, false,
            QStringLiteral("Choose an absolute custom mpv configuration directory.") };
    }
    const QFileInfo info { cleaned };
    if (!info.exists() || !info.isDir() || !info.isReadable()) {
        return { MpvConfigPolicy::Mode::Disabled, {}, false,
            QStringLiteral("The custom mpv configuration directory must exist and be readable.") };
    }
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty()) {
        return { MpvConfigPolicy::Mode::Disabled, {}, false,
            QStringLiteral("The custom mpv configuration directory cannot be resolved.") };
    }
    return { MpvConfigPolicy::Mode::Custom, canonical, true, {} };
}

} // namespace Spool
