#include "BrowseIndex.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Spool {

namespace {

    constexpr int kLetters = 27;

    // How leading letters divide a title list once articles are dropped, as a
    // share of the whole: the starting guess for a letter nobody has loaded.
    // S is the giant; Q and X barely register.
    constexpr std::array<double, kLetters> kPrior { 2.5, 6.5, 6.5, 5.5, 6.0, 3.0, 3.5, 3.5, 5.0, 3.0, 1.8, 2.0, 4.5,
        6.5, 2.5, 1.8, 4.0, 0.3, 4.0, 10.0, 6.0, 1.0, 1.3, 4.0, 0.3, 0.8, 0.8 };

    QString estimateLabel(int index)
    {
        return index == 0 ? QStringLiteral("#") : QString(QChar(u'A' + index - 1));
    }

    int estimateIndex(const QString& label)
    {
        if (label == QLatin1String("#"))
            return 0;
        if (label.size() == 1 && label.at(0) >= u'A' && label.at(0) <= u'Z')
            return label.at(0).unicode() - u'A' + 1;
        return -1;
    }

    // "#" before every letter, letters in code-point order otherwise.
    bool labelBefore(const QString& left, const QString& right)
    {
        if (left == right)
            return false;
        if (left == QLatin1String("#"))
            return true;
        if (right == QLatin1String("#"))
            return false;
        return left < right;
    }

} // namespace

QString BrowseIndex::labelFor(const QString& sortName)
{
    for (const QChar character : sortName) {
        if (character.isSpace())
            continue;
        const QString base = QString(character).normalized(QString::NormalizationForm_D);
        const QChar letter = base.isEmpty() ? character : base.at(0);
        if (!letter.isLetter())
            return QStringLiteral("#");
        return QString(letter).toUpper();
    }
    return QStringLiteral("#");
}

QString BrowseIndex::sortKey(const QString& text)
{
    const QString decomposed = text.normalized(QString::NormalizationForm_D);
    QString key;
    key.reserve(decomposed.size());
    for (const QChar character : decomposed) {
        if (character.category() != QChar::Mark_NonSpacing)
            key.append(character);
    }
    return key.toCaseFolded().simplified();
}

void BrowseIndex::reset(int count, bool descending)
{
    m_count = std::max(0, count);
    m_descending = descending;
    m_exactLabels.clear();
    m_exactCounts.clear();
    m_firstSeen.assign(kLetters, -1);
    m_lastSeen.assign(kLetters, -1);
    m_dirty = true;
}

void BrowseIndex::setExact(const std::vector<BrowseIndexEntry>& entries)
{
    m_exactLabels.clear();
    m_exactCounts.clear();
    for (const BrowseIndexEntry& entry : entries) {
        if (entry.count <= 0 || entry.label.isEmpty())
            continue;
        const QString label = labelFor(entry.label);
        // Servers may split accented letters out; they share a letter here.
        if (!m_exactLabels.isEmpty() && m_exactLabels.last() == label) {
            m_exactCounts.back() += entry.count;
            continue;
        }
        m_exactLabels.push_back(label);
        m_exactCounts.push_back(entry.count);
    }
    m_dirty = true;
}

void BrowseIndex::observe(int row, const QString& sortName)
{
    if (row < 0 || row >= m_count || m_firstSeen.empty())
        return;
    const int index = estimateIndex(labelFor(sortName));
    if (index < 0)
        return;
    int& first = m_firstSeen[static_cast<size_t>(index)];
    int& last = m_lastSeen[static_cast<size_t>(index)];
    if (first >= 0 && first <= row && last >= row)
        return;
    first = first < 0 ? row : std::min(first, row);
    last = std::max(last, row);
    if (!exact())
        m_dirty = true;
}

const std::vector<BrowseIndex::Letter>& BrowseIndex::letters() const
{
    if (m_dirty)
        rebuild();
    return m_letters;
}

void BrowseIndex::rebuild() const
{
    m_dirty = false;
    m_letters.clear();
    if (m_count <= 0)
        return;

    if (exact()) {
        const qsizetype size = m_exactLabels.size();
        long long sum = 0;
        for (const int count : m_exactCounts)
            sum += count;
        // Counts that disagree with the listing (a library changing under
        // us) are scaled to fit rather than trusted row for row.
        const double scale = sum > 0 ? static_cast<double>(m_count) / static_cast<double>(sum) : 1.0;
        const bool matches = sum == m_count;
        long long cumulative = 0;
        for (qsizetype position = 0; position < size; ++position) {
            const qsizetype ascending = m_descending ? size - 1 - position : position;
            const int row = std::min(m_count - 1, static_cast<int>(std::llround(cumulative * scale)));
            m_letters.push_back({ m_exactLabels.at(ascending), row, matches });
            cumulative += m_exactCounts[static_cast<size_t>(ascending)];
        }
        return;
    }

    // Estimate: everything in listing order.
    std::array<double, kLetters> weight {};
    std::array<int, kLetters> first {};
    std::array<int, kLetters> last {};
    for (int position = 0; position < kLetters; ++position) {
        const int ascending = m_descending ? kLetters - 1 - position : position;
        weight[position] = kPrior[ascending];
        first[position] = m_firstSeen[static_cast<size_t>(ascending)];
        last[position] = m_lastSeen[static_cast<size_t>(ascending)];
    }
    int previous = 0;
    for (int k = 0; k < kLetters; ++k) {
        // The nearest pinned rows either side of where letter k begins.
        int lo = -1;
        int a = -1;
        for (int j = 0; j < k; ++j) {
            if (last[j] > lo) {
                lo = last[j];
                a = j;
            }
        }
        int hi = m_count;
        int b = kLetters;
        for (int j = k; j < kLetters; ++j) {
            if (first[j] >= 0 && first[j] < hi) {
                hi = first[j];
                b = j;
            }
        }
        int row = 0;
        bool exactRow = false;
        if (hi <= lo + 1) {
            row = hi;
            exactRow = hi == lo + 1;
        } else {
            const double startShare = a >= 0 ? weight[a] / 2 : 0.0;
            const double endShare = b < kLetters ? weight[b] / 2 : 0.0;
            double before = startShare;
            for (int j = a + 1; j < k; ++j)
                before += weight[j];
            double total = before + endShare;
            for (int j = std::max(a + 1, k); j < b; ++j)
                total += weight[j];
            const double share = total > 0 ? before / total : 0.0;
            row = lo + 1 + static_cast<int>(std::lround(share * (hi - lo - 1)));
        }
        row = std::clamp(std::max(row, previous), 0, m_count);
        previous = row;
        m_letters.push_back(
            { estimateLabel(m_descending ? kLetters - 1 - k : k), std::min(row, m_count - 1), exactRow });
    }
}

int BrowseIndex::labelIndex(const QString& label) const
{
    const auto& all = letters();
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i].label == label)
            return static_cast<int>(i);
    }
    return -1;
}

QString BrowseIndex::labelAt(int row) const
{
    const auto& all = letters();
    if (all.empty())
        return QStringLiteral("#");
    // The last letter starting at or before the row: of several letters
    // starting on one row, all but the last are empty.
    const auto after = std::upper_bound(
        all.begin(), all.end(), row, [](int value, const Letter& letter) { return value < letter.row; });
    return after == all.begin() ? all.front().label : std::prev(after)->label;
}

int BrowseIndex::rowForLabel(const QString& label) const
{
    const auto& all = letters();
    if (all.empty())
        return -1;
    const int found = labelIndex(label);
    if (found >= 0)
        return all[static_cast<size_t>(found)].row;
    for (const Letter& letter : all) {
        const bool after = m_descending ? labelBefore(letter.label, label) : labelBefore(label, letter.label);
        if (after)
            return letter.row;
    }
    return m_count - 1;
}

std::pair<int, int> BrowseIndex::spanForLabel(const QString& label) const
{
    const auto& all = letters();
    const int start = rowForLabel(label);
    if (start < 0)
        return { 0, m_count };
    const int found = labelIndex(label);
    if (found < 0)
        return { start, start };
    const size_t next = static_cast<size_t>(found) + 1;
    return { start, next < all.size() ? std::max(start, all[next].row) : m_count };
}

} // namespace Spool
