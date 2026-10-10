#include "MobileTestFixtures.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

namespace SpoolTests {
namespace {
    QString extractedRoot;
}

const QString& mobileFixtureRoot()
{
    if (extractedRoot.isEmpty())
        qFatal("mobile test fixtures have not been prepared");
    return extractedRoot;
}

bool prepareMobileFixtures()
{
    static QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/spool-test-fixtures-XXXXXX"));
    if (!directory.isValid()) {
        std::fprintf(stderr, "mobile tests: cannot create isolated fixture directory\n");
        return false;
    }
    const QString prefix = QStringLiteral(":/spool-mobile-fixtures/");
    QDirIterator fixtures(prefix, QDir::Files, QDirIterator::Subdirectories);
    while (fixtures.hasNext()) {
        const QString source = fixtures.next();
        const QString destination = directory.filePath(source.mid(prefix.size()));
        if (!QDir().mkpath(QFileInfo(destination).absolutePath()) || !QFile::copy(source, destination)) {
            std::fprintf(stderr, "mobile tests: cannot extract packaged fixture\n");
            return false;
        }
    }
    qputenv("QML_DISABLE_DISK_CACHE", "1");
    extractedRoot = directory.path();
    return true;
}

} // namespace SpoolTests
