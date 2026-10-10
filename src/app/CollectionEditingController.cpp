#include "CollectionEditingController.h"

#include "../common/AsyncTask.h"
#include "../provider/SourceHub.h"
#include <QPointer>
#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace Spool {
namespace {
    constexpr int MaximumPages = 256;
    constexpr int MaximumEntries = 10000;
}

CollectionEditingController::CollectionEditingController(SourceHub *sources, QObject *parent)
    : QObject(parent)
    , m_sources(sources)
{
    connect(sources, &SourceHub::capabilitySupportChanged, this, [this](const QString& account) {
        if (!m_containerId.isEmpty() && account == m_accountId) {
            // A permission/offer or source generation change invalidates every
            // loaded occurrence and in-flight result, not just the buttons.
            const auto container = m_containerId;
            const auto title = m_title;
            close();
            if (m_sources->collectionEditingAvailable(container))
                open(container, title);
            else {
                m_problem = tr("Collection editing is unavailable for this account.");
                emit changed();
            }
        }
    });
}

CollectionEditingController::~CollectionEditingController()
{
    if (!m_containerId.isEmpty())
        m_sources->cancelCollection(m_containerId);
}

int CollectionEditingController::indexOf(const QString& entryId) const
{
    for (size_t i = 0; i < m_entries.size(); ++i)
        if (m_entries[i].playlistItemId == entryId)
            return int(i);
    return -1;
}

int CollectionEditingController::selectedIndex() const
{
    return indexOf(m_selected);
}

void CollectionEditingController::clearEntries()
{
    m_entries.clear();
    m_rows.clear();
    m_selected.clear();
    m_seenCursors.clear();
    m_cursor.reset();
    m_pages = 0;
    m_exhausted = true;
    m_removable = false;
    m_movable = false;
}

void CollectionEditingController::close()
{
    ++m_generation;
    if (!m_containerId.isEmpty())
        m_sources->cancelCollection(m_containerId);
    m_containerId.clear();
    m_accountId.clear();
    m_title.clear();
    m_problem.clear();
    m_busy = false;
    clearEntries();
    emit changed();
}

void CollectionEditingController::open(const QString& containerId, const QString& title)
{
    close();
    m_containerId = containerId;
    m_accountId = m_sources->accountOf(containerId);
    m_title = title;
    if (!m_sources->collectionEditingAvailable(containerId)) {
        m_problem = tr("Collection editing is unavailable for this account.");
        emit changed();
        return;
    }
    beginLoad(true);
}

void CollectionEditingController::selectEntry(const QString& entryId)
{
    if (m_selected != entryId && indexOf(entryId) >= 0) {
        m_selected = entryId;
        emit changed();
    }
}

void CollectionEditingController::refresh()
{
    beginLoad(true);
}
void CollectionEditingController::requestNextPage()
{
    if (hasMore())
        beginLoad(false);
}

void CollectionEditingController::beginLoad(bool reset)
{
    if (m_busy || m_containerId.isEmpty())
        return;
    m_busy = true;
    m_problem.clear();
    const auto generation = ++m_generation;
    const int desired = reset ? std::max(1, int(m_entries.size())) : int(m_entries.size()) + 1;
    emit changed();
    Async::runScoped(
        this, load(generation, reset, desired), [this, generation] { finish(generation); },
        [this, generation](const std::exception_ptr&) { fail(generation); }, "collection entries");
}

void CollectionEditingController::finish(quint64 generation)
{
    if (generation != m_generation)
        return;
    m_busy = false;
    emit changed();
}

void CollectionEditingController::fail(quint64 generation)
{
    if (generation != m_generation)
        return;
    // Never leave stale editable entries after an uncertain read or mutation.
    clearEntries();
    m_busy = false;
    m_problem = tr("Could not refresh the collection. Retry before making another change.");
    emit changed();
}

QCoro::Task<void> CollectionEditingController::load(quint64 generation, bool reset, int desiredCount)
{
    QPointer<CollectionEditingController> guard(this);
    const QString container = m_containerId;
    bool removable = m_removable;
    bool movable = m_movable;
    if (reset) {
        const auto info = co_await m_sources->collectionCall(container, QStringLiteral("collectionInfo"));
        if (!guard || generation != m_generation)
            co_return;
        const QString mode = info.value(QStringLiteral("moveMode")).toString();
        if (mode != QStringLiteral("none") && mode != QStringLiteral("index") && mode != QStringLiteral("after"))
            throw std::runtime_error("invalid_collection_info");
        if (info.value(QStringLiteral("ordered")).metaType().id() != QMetaType::Bool
            || info.value(QStringLiteral("removable")).metaType().id() != QMetaType::Bool)
            throw std::runtime_error("invalid_collection_info");
        removable = info.value(QStringLiteral("removable")).toBool();
        movable = info.value(QStringLiteral("ordered")).toBool() && mode != QStringLiteral("none");
    }
    auto cursor = reset ? std::optional<QString>() : m_cursor;
    auto cursors = reset ? QSet<QString>() : m_seenCursors;
    const size_t baseCount = reset ? 0 : m_entries.size();
    std::vector<MovieItem> entries;
    int pages = reset ? 0 : m_pages;
    bool exhausted = reset ? false : m_exhausted;
    QSet<QString> entryIds;
    if (!reset)
        for (const auto& item : m_entries)
            entryIds.insert(item.playlistItemId);
    while (!exhausted && int(baseCount + entries.size()) < desiredCount) {
        if (pages >= MaximumPages || baseCount + entries.size() >= MaximumEntries)
            throw std::runtime_error("response_limit");
        auto page = co_await m_sources->collectionEntries(container, cursor);
        if (!guard || generation != m_generation)
            co_return;
        ++pages;
        if (!page.exhausted && (!page.nextCursor || page.nextCursor->isEmpty() || cursors.contains(*page.nextCursor)))
            throw std::runtime_error("invalid_pagination");
        if (!page.exhausted)
            cursors.insert(*page.nextCursor);
        if (baseCount + entries.size() + page.items.size() > MaximumEntries)
            throw std::runtime_error("response_limit");
        for (auto& item : page.items) {
            if (item.playlistItemId.isEmpty() || entryIds.contains(item.playlistItemId))
                throw std::runtime_error("invalid_collection_entry");
            entryIds.insert(item.playlistItemId);
            entries.push_back(std::move(item));
        }
        cursor = page.exhausted ? std::nullopt : page.nextCursor;
        exhausted = page.exhausted;
    }
    const QString selected = m_selected;
    const int nearest = std::max(0, selectedIndex());
    if (reset) {
        m_entries = std::move(entries);
        m_rows.clear();
    } else {
        m_entries.insert(
            m_entries.end(), std::make_move_iterator(entries.begin()), std::make_move_iterator(entries.end()));
    }
    m_cursor = std::move(cursor);
    m_seenCursors = std::move(cursors);
    m_pages = pages;
    m_exhausted = exhausted;
    m_removable = removable;
    m_movable = movable;
    m_selected = indexOf(selected) >= 0 ? selected
        : m_entries.empty()             ? QString()
                                        : m_entries[std::min(nearest, int(m_entries.size()) - 1)].playlistItemId;
    m_rows.reserve(qsizetype(m_entries.size()));
    for (size_t index = baseCount; index < m_entries.size(); ++index) {
        const auto& item = m_entries[index];
        m_rows.append(
            QVariantMap { { QStringLiteral("entryId"), item.playlistItemId }, { QStringLiteral("itemId"), item.id },
                { QStringLiteral("title"), item.title }, { QStringLiteral("itemType"), item.itemType } });
    }
    emit changed();
}

void CollectionEditingController::removeEntry(const QString& entryId)
{
    mutate(entryId, 0);
}
void CollectionEditingController::moveEntry(const QString& entryId, int direction)
{
    if (direction == -1 || direction == 1)
        mutate(entryId, direction);
}

void CollectionEditingController::mutate(const QString& entryId, int direction)
{
    if (m_busy || indexOf(entryId) < 0 || (direction == 0 ? !m_removable : !m_movable))
        return;
    m_selected = entryId;
    m_busy = true;
    m_problem.clear();
    const auto generation = ++m_generation;
    emit changed();
    Async::runScoped(
        this, mutation(generation, entryId, direction), [this, generation] { finish(generation); },
        [this, generation](const std::exception_ptr&) { fail(generation); }, "collection mutation");
}

QCoro::Task<void> CollectionEditingController::mutation(quint64 generation, QString entryId, int direction)
{
    QPointer<CollectionEditingController> guard(this);
    if (direction == 1 && indexOf(entryId) == int(m_entries.size()) - 1 && !m_exhausted) {
        // An anchor on the next page must be known before sending a move. No
        // full-container lookup and no index-to-anchor translation request.
        co_await load(generation, false, int(m_entries.size()) + 1);
        if (!guard || generation != m_generation)
            co_return;
    }
    const int from = indexOf(entryId);
    const int destination = from + direction;
    if (from < 0 || (direction != 0 && (destination < 0 || destination >= int(m_entries.size()))))
        co_return;
    const QString container = m_containerId;
    const int desired = std::max(1, int(m_entries.size()));
    QVariantMap args { { QStringLiteral("entryId"), entryId } };
    if (direction != 0) {
        args.insert(QStringLiteral("index"), destination);
        // Both forms describe the destination AFTER removing the moving row.
        const int predecessor = direction < 0 ? destination - 1 : destination;
        // An invalid QVariant becomes JS undefined; the wire contract requires null.
        args.insert(QStringLiteral("afterEntryId"),
            predecessor < 0 ? QVariant::fromValue(nullptr) : QVariant(m_entries[size_t(predecessor)].playlistItemId));
    }
    bool uncertain = false;
    bool permissionDenied = false;
    try {
        co_await m_sources->collectionCall(container,
            direction == 0 ? QStringLiteral("collectionRemove") : QStringLiteral("collectionMove"), std::move(args));
    } catch (const std::exception& error) {
        uncertain = true;
        const auto code = QString::fromUtf8(error.what());
        permissionDenied
            = code.contains(QStringLiteral("http_403")) || code.contains(QStringLiteral("permission_denied"));
    }
    if (!guard || generation != m_generation)
        co_return;
    m_sources->collectionChanged(container);
    // Re-read permissions as well as rows after success, 403, disappearance or
    // an uncertain transport error. Never retry a mutation automatically.
    co_await load(generation, true, desired);
    if (!guard || generation != m_generation)
        co_return;
    if (uncertain)
        m_problem = permissionDenied
            ? tr("This account does not have permission to make that change. Entries and permissions have been "
                 "refreshed.")
            : tr("The change could not be confirmed. Entries and permissions have been refreshed.");
}

} // namespace Spool
