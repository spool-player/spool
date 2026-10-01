#include "models/LibraryListModel.h"
#include "TestMain.h"

#include <QDebug>
#include <QPersistentModelIndex>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdlib>

namespace {
void require(bool condition, const char *message)
{
    if (!condition) {
        qCritical() << message;
        std::exit(EXIT_FAILURE);
    }
}
Spool::LibraryItem library(const char *id)
{
    Spool::LibraryItem result;
    result.id = QString::fromLatin1(id);
    return result;
}
void requireOrder(const Spool::LibraryListModel& model, const QStringList& expected)
{
    QStringList actual;
    for (const auto& item : model.libraries())
        actual.append(item.id);
    require(actual == expected, "library order mismatch");
}
}

SPOOL_TEST_MAIN("library-list-model")
{
    QTemporaryDir settingsDir;
    require(settingsDir.isValid(), "temporary settings directory");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QSettings().clear();
    const auto a = library("aaaaaaaa:movies");
    const auto b = library("bbbbbbbb:movies");
    const auto c = library("aaaaaaaa:music");
    const auto d = library("bbbbbbbb:photos");
    Spool::LibraryListModel model;
    model.setLibraries({ a, b, c });
    const QPersistentModelIndex selected(model.index(0));
    require(model.moveLibrary(0, 2), "forward move");
    require(selected.row() == 2 && selected.data(Spool::LibraryListModel::IdRole).toString() == a.id,
        "selected library follows forward move");
    requireOrder(model, { b.id, c.id, a.id });
    require(model.moveLibrary(2, 0), "backward move");
    requireOrder(model, { a.id, b.id, c.id });
    require(selected.row() == 0 && selected.data(Spool::LibraryListModel::IdRole).toString() == a.id,
        "selected library follows backward move");
    require(
        !model.moveLibrary(-1, 0) && !model.moveLibrary(0, 3) && !model.moveLibrary(1, 1), "invalid moves rejected");
    requireOrder(model, { a.id, b.id, c.id });
    model.setLibraries({ c, a });
    require(model.moveLibrary(1, 0), "move while account disconnected");
    model.clear();
    Spool::LibraryListModel restored;
    restored.setLibraries({ d, a, c, b });
    requireOrder(restored, { c.id, b.id, a.id, d.id });
    return EXIT_SUCCESS;
}
