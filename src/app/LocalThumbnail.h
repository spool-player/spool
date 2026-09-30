#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>

namespace Spool {
// Called on the dedicated thumbnail worker, never the GUI or network thread.
QByteArray localThumbnail(const QUrl& url, const QString& cacheDirectory, QString& error);
}
