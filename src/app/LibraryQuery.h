#pragma once

#include "../media/MediaTypes.h"

#include <QStringList>
#include <QVariantMap>

namespace Spool {

QString libraryContentLabel(const LibraryItem& library);
bool supportsLatestLibraryRow(const LibraryItem& library);
QVariantMap defaultLibraryQuery(const LibraryItem& library);
QStringList libraryQueryStringList(const QVariantMap& query, const QString& key);
int activeLibraryFilterCount(const QVariantMap& query);
QString libraryCacheKey(const LibraryItem& library);
QString libraryCacheKey(const LibraryItem& library, const QVariantMap& query);

} // namespace Spool
