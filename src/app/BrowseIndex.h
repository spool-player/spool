#pragma once

#include "../media/MediaTypes.h"

#include <QString>
#include <QStringList>

#include <utility>
#include <vector>

namespace Spool {

// Where each leading letter starts in a name-sorted listing that may be
// mostly unloaded. With the server's counts the answer is exact. Without
// them it starts from how film and series titles usually divide across the
// alphabet and corrects itself from every row that arrives: a row whose
// letter is known pins that letter's span, and the unknown stretches between
// pinned rows are shared out by the same prior. A jump to an estimated
// letter therefore loads the rows that make the next estimate better.
class BrowseIndex final {
public:
    struct Letter {
        QString label;
        int row = 0;
        bool exact = false;
    };

    // A fresh listing of `count` rows, ascending or descending by name.
    void reset(int count, bool descending);
    int count() const
    {
        return m_count;
    }
    bool descending() const
    {
        return m_descending;
    }
    // The server's counts per leading character, ascending ("#" first).
    void setExact(const std::vector<BrowseIndexEntry>& entries);
    bool exact() const
    {
        return !m_exactLabels.isEmpty();
    }
    // A loaded row's sort name. Rows may be observed in any order and more
    // than once; what was learnt outlives the row being unloaded again.
    void observe(int row, const QString& sortName);

    // Letters in listing order with the row each starts at. Letters with no
    // rows are left out when exact and kept (zero width) when estimated.
    const std::vector<Letter>& letters() const;
    // The letter a row is under, known or estimated.
    QString labelAt(int row) const;
    // First row of a letter, or of the nearest letter after it in listing
    // order when it has none; -1 when the letter is unknown to the index.
    int rowForLabel(const QString& label) const;
    // The rows [first, end) a letter spans, estimated or exact.
    std::pair<int, int> spanForLabel(const QString& label) const;

    // "#" for anything that does not start with a letter; letters are
    // upper-cased with accents removed, so "Émilie" files under "E".
    static QString labelFor(const QString& sortName);
    // Lower-cased, accent-free comparison form of a sort name or typed text.
    static QString sortKey(const QString& text);

private:
    void rebuild() const;
    int labelIndex(const QString& label) const;

    int m_count = 0;
    bool m_descending = false;
    // Ascending labels and the rows of each, from the server.
    QStringList m_exactLabels;
    std::vector<int> m_exactCounts;
    // Per ascending estimate label: first and last observed row, or -1.
    std::vector<int> m_firstSeen;
    std::vector<int> m_lastSeen;
    mutable bool m_dirty = true;
    mutable std::vector<Letter> m_letters;
};

} // namespace Spool
