#pragma once

#include <QByteArray>
#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

namespace Spool {

// Roku BIF v0: timestamps are index values multiplied by the header's
// millisecond multiplier; the terminal entry supplies the last JPEG boundary.
class BifSequence final {
public:
    bool parse(QByteArray bytes, QString *error);
    int frameAt(double seconds) const;
    int count() const
    {
        return m_entries.size();
    }
    // Returns shared owned backing plus the selected JPEG range, without
    // copying that range. The worker retains backing while borrowing bytes.
    QByteArray frameBacking(int index, qsizetype *offset, qsizetype *length) const;
    QSize frameSize(int index) const;

private:
    struct Entry {
        quint64 timeMs;
        quint32 offset;
        quint32 end;
        mutable QSize size;
    };
    QByteArray m_bytes;
    QVector<Entry> m_entries;
};

inline constexpr qint64 TrickplayDecodedByteBudget = 50'000'000;

// Resident sheets are delivered intact; oversized JPEG sheets use codec-side
// clipping. Expected geometry is checked before decoding either representation.
QImage decodeTrickplayFrame(
    const QByteArray& bytes, const QRect& crop, const QSize& expectedSize, bool residentSheet, QString *error);

} // namespace Spool
