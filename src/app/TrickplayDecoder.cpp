#include "TrickplayDecoder.h"

#include <QBuffer>
#include <QImageReader>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <limits>

namespace Spool {

bool BifSequence::parse(QByteArray bytes, QString *error)
{
    m_bytes.clear();
    m_entries.clear();
    auto fail = [error](const char *message) {
        if (error)
            *error = QLatin1String(message);
        return false;
    };
    if (bytes.size() < 64 || bytes.first(8) != QByteArray::fromHex("894249460d0a1a0a"))
        return fail("Invalid BIF signature or truncated header");
    const auto word = [&bytes](qsizetype offset) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + offset));
    };
    if (word(8) != 0)
        return fail("Unsupported BIF version");
    const quint32 count = word(12);
    const quint64 tableEnd = 64 + (quint64(count) + 1) * 8;
    if (!count || count > 1000000 || tableEnd > quint64(bytes.size()))
        return fail("Invalid or truncated BIF index");
    const quint64 multiplier = word(16) ? word(16) : 1000;
    if (word(64 + qsizetype(count) * 8) != std::numeric_limits<quint32>::max())
        return fail("Missing BIF terminal entry");
    QVector<Entry> entries;
    entries.reserve(int(count));
    quint64 previousTime = 0;
    for (quint32 i = 0; i < count; ++i) {
        const qsizetype position = 64 + qsizetype(i) * 8;
        const quint32 timestamp = word(position);
        const quint32 offset = word(position + 4);
        const quint32 end = word(position + 12);
        const quint64 time = quint64(timestamp) * multiplier;
        if (timestamp == std::numeric_limits<quint32>::max() || (i && time <= previousTime) || offset < tableEnd
            || end <= offset || end > quint64(bytes.size()))
            return fail("Invalid BIF timestamp or image boundary");
        entries.push_back({ time, offset, end, {} });
        previousTime = time;
    }
    m_entries = std::move(entries);
    m_bytes = std::move(bytes);
    if (error)
        error->clear();
    return true;
}

int BifSequence::frameAt(double seconds) const
{
    if (m_entries.isEmpty() || !std::isfinite(seconds))
        return -1;
    const double milliseconds = std::max(0.0, seconds) * 1000.0;
    const auto it = std::upper_bound(m_entries.cbegin(), m_entries.cend(), milliseconds,
        [](double value, const Entry& entry) { return value < entry.timeMs; });
    return it == m_entries.cbegin() ? 0 : int(it - m_entries.cbegin() - 1);
}

QByteArray BifSequence::frameBacking(int index, qsizetype *offset, qsizetype *length) const
{
    *offset = 0;
    *length = 0;
    if (index < 0 || index >= m_entries.size())
        return {};
    const Entry& entry = m_entries[index];
    *offset = entry.offset;
    *length = entry.end - entry.offset;
    return m_bytes;
}

QSize BifSequence::frameSize(int index) const
{
    if (index < 0 || index >= m_entries.size())
        return {};
    const Entry& entry = m_entries[index];
    if (!entry.size.isValid()) {
        // Header inspection borrows sequence bytes synchronously. Workers
        // retain shared backing when borrowing the same JPEG range.
        QBuffer buffer;
        buffer.setData(QByteArray::fromRawData(m_bytes.constData() + entry.offset, entry.end - entry.offset));
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        entry.size = reader.size();
    }
    return entry.size;
}

QImage decodeTrickplayFrame(
    const QByteArray& bytes, const QRect& crop, const QSize& expectedSize, bool residentSheet, QString *error)
{
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(false);
    const QSize size = reader.size();
    const qint64 pixels = qint64(size.width()) * size.height();
    if (!size.isValid() || size != expectedSize || pixels > 64 * 1024 * 1024
        || (reader.format() != QByteArrayLiteral("jpeg") && pixels * 4 > TrickplayDecodedByteBudget)
        || (crop.isValid()
            && (!QRect(QPoint(), size).contains(crop) || qint64(crop.width()) * crop.height() > 1024 * 1024))
        || (!crop.isValid() && pixels > 1024 * 1024) || (residentSheet && pixels * 4 > TrickplayDecodedByteBudget)) {
        if (error)
            *error = QStringLiteral("Invalid or oversized preview dimensions");
        return {};
    }
    if (crop.isValid() && !residentSheet)
        reader.setClipRect(crop);
    QImage image = reader.read();
    const QSize outputSize = crop.isValid() && !residentSheet ? crop.size() : size;
    if (image.isNull() || image.size() != outputSize) {
        if (error)
            *error = image.isNull() ? reader.errorString() : QStringLiteral("Invalid decoded preview dimensions");
        return {};
    }
    // JPEG normally already produces RGB32. Other supported small images use
    // the same byte accounting and texture layout.
    if (image.format() != QImage::Format_RGB32)
        image = image.convertToFormat(QImage::Format_RGB32);
    return image;
}

} // namespace Spool
