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
    for (int row = 0; row < model.count(); ++row) {
        const QString id = model.libraryAt(row).id;
        actual.append(id);
        require(model.get(row).value(QStringLiteral("libraryId")).toString() == id
                && model.data(model.index(row), Spool::LibraryListModel::IdRole).toString() == id,
            "visible row lookup, role and navigation must agree");
    }
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

    const QPersistentModelIndex kept(restored.index(2));
    require(restored.hideLibrary(b.id), "hide a scoped library");
    require(!restored.hideLibrary(b.id) && !restored.hideLibrary(QStringLiteral("unknown")),
        "duplicate and unknown hides are rejected");
    requireOrder(restored, { c.id, a.id, d.id });
    require(kept.row() == 1 && kept.data(Spool::LibraryListModel::IdRole).toString() == a.id,
        "selection follows removal of a preceding hidden library");
    require(restored.libraries().size() == 4 && restored.libraryById(b.id).id == b.id,
        "hidden libraries remain available to refresh and ID navigation");
    require(
        restored.isHidden(b.id) && !restored.isHidden(a.id), "same native ID on another account must remain visible");
    require(restored.hiddenLibraries().size() == 1
            && restored.hiddenLibraries().first().toMap().value(QStringLiteral("libraryId")).toString() == b.id,
        "hidden management exposes the scoped ID");
    require(restored.moveLibrary(0, 2), "reorder around a hidden library");
    requireOrder(restored, { a.id, d.id, c.id });
    restored.clear();
    require(restored.hiddenLibraries().isEmpty(), "disconnected libraries are not offered for restoration");
    Spool::LibraryListModel reloaded;
    reloaded.setLibraries({ a, b, c, d });
    requireOrder(reloaded, { a.id, d.id, c.id });
    require(reloaded.isHidden(b.id), "hidden preference survives reconstruction and refresh");
    require(reloaded.showLibrary(b.id), "restore an individual library");
    requireOrder(reloaded, { a.id, b.id, d.id, c.id });
    require(!reloaded.showLibrary(b.id) && !reloaded.showLibrary(QStringLiteral("unknown")),
        "visible and unknown libraries cannot be restored twice");
    for (const auto& id : { a.id, b.id, c.id, d.id })
        require(reloaded.hideLibrary(id), "hide every library");
    require(reloaded.count() == 0 && reloaded.hiddenLibraries().size() == 4 && reloaded.libraries().size() == 4,
        "hiding all rows retains the data and restoration choices");
    Spool::LibraryListModel allHidden;
    allHidden.setLibraries({ a, b, c, d });
    require(allHidden.count() == 0, "all-hidden state survives restart");
    require(allHidden.showLibrary(b.id), "restore one library from all-hidden state");
    requireOrder(allHidden, { b.id });
    Spool::LibraryListModel individuallyRestored;
    individuallyRestored.setLibraries({ a, b, c, d });
    requireOrder(individuallyRestored, { b.id });
    require(individuallyRestored.hiddenLibraries().size() == 3, "individual unhide is persisted");
    return EXIT_SUCCESS;
}
