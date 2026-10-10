#pragma once

#include <QByteArray>
#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

#include <array>
#include <memory>
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
// Conservative reservation includes raw MCU padding for small/odd dimensions;
// RGB-equivalent charging prevents the planar cutover expanding the cache.
inline qint64 trickplayTextureCost(const QSize& size)
{
    const qint64 w = size.width(), h = size.height();
    if (w <= 0 || h <= 0)
        return 0;
    const qint64 groups = (h + 15) / 16;
    const qint64 raw = ((w + 7) / 8 * 8) * groups * 16 + 2 * (((w + 1) / 2 + 7) / 8 * 8) * groups * 8;
    return qMax(w * h * 4, raw);
}

// Immutable after publication. Raw planes include MCU padding in their owned
// backing, but only the visible dimensions are uploaded.
struct TrickplayTexture {
    QSize size;
    QImage rgb;
    std::array<QByteArray, 3> planes;
    std::array<int, 3> strides {};
    bool planar() const
    {
        return !planes[0].isEmpty();
    }
    QImage image() const;
};

std::shared_ptr<const TrickplayTexture> decodeTrickplayTexture(const QByteArray& bytes, const QRect& crop,
    const QSize& expectedSize, bool residentSheet, bool accurate, QString *error);

// Resident sheets are delivered intact; oversized JPEG sheets use codec-side
// clipping. Expected geometry is checked before decoding either representation.
QImage decodeTrickplayFrame(
    const QByteArray& bytes, const QRect& crop, const QSize& expectedSize, bool residentSheet, QString *error);

} // namespace Spool
