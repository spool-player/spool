#include "TrickplayDecoder.h"

#include <QBuffer>
#include <QImageReader>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <limits>
#if defined(SPOOL_QT_BUNDLED_JPEG)
#include <QtJpeg/private/jpeglib.h>
#else
#include <jpeglib.h>
#endif

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

namespace {
    struct RawDecoder {
        jpeg_decompress_struct codec {};
        jpeg_error_mgr error {};
        std::jmp_buf jump;
        bool created = false;
        static void fail(j_common_ptr codec)
        {
            auto *self = reinterpret_cast<RawDecoder *>(codec);
            std::longjmp(self->jump, 1);
        }
        ~RawDecoder()
        {
            if (created)
                jpeg_destroy_decompress(&codec);
        }
    };
}

std::shared_ptr<const TrickplayTexture> decodeTrickplayTexture(const QByteArray& bytes, const QRect& crop,
    const QSize& expectedSize, bool residentSheet, bool accurate, QString *error)
{
    auto frame = std::make_shared<TrickplayTexture>();
    // Keep the RGB32 admission envelope, even though raw output is smaller.
    // Oversized sheets remain on Qt's codec-clipped RGB path.
    const qint64 pixels = qint64(expectedSize.width()) * expectedSize.height();
    const bool rawEligible = expectedSize.isValid() && trickplayTextureCost(expectedSize) <= TrickplayDecodedByteBudget
        && (residentSheet || !crop.isValid()) && (crop.isValid() || pixels <= 1024 * 1024)
        && (!crop.isValid()
            || (QRect(QPoint(), expectedSize).contains(crop) && qint64(crop.width()) * crop.height() <= 1024 * 1024));
    if (rawEligible && bytes.size() >= 2 && uchar(bytes[0]) == 0xff && uchar(bytes[1]) == 0xd8) {
        // Heap-owned state and output survive libjpeg's error longjmp without
        // bypassing destructors or reading modified automatic variables.
        auto decoder = std::make_unique<RawDecoder>();
        decoder->codec.err = jpeg_std_error(&decoder->error);
        decoder->error.error_exit = RawDecoder::fail;
        if (setjmp(decoder->jump) == 0) {
            jpeg_create_decompress(&decoder->codec);
            decoder->created = true;
            jpeg_mem_src(&decoder->codec, reinterpret_cast<const unsigned char *>(bytes.constData()),
                static_cast<unsigned long>(bytes.size()));
            jpeg_read_header(&decoder->codec, TRUE);
            auto& codec = decoder->codec;
            if (codec.data_precision == 8 && codec.jpeg_color_space == JCS_YCbCr && codec.num_components == 3
                && codec.image_width == unsigned(expectedSize.width())
                && codec.image_height == unsigned(expectedSize.height()) && codec.comp_info[0].h_samp_factor == 2
                && codec.comp_info[0].v_samp_factor == 2 && codec.comp_info[1].h_samp_factor == 1
                && codec.comp_info[1].v_samp_factor == 1 && codec.comp_info[2].h_samp_factor == 1
                && codec.comp_info[2].v_samp_factor == 1) {
                codec.raw_data_out = TRUE;
                codec.out_color_space = JCS_YCbCr;
                codec.dct_method = accurate ? JDCT_ISLOW : JDCT_IFAST;
                jpeg_start_decompress(&codec);
                frame->size = expectedSize;
                const int groups = (expectedSize.height() + 15) / 16;
                for (int i = 0; i < 3; ++i) {
                    const auto& component = codec.comp_info[i];
                    // No scaled IDCT: verify the coded and reconstructed shapes.
                    const int divisor = i == 0 ? 1 : 2;
                    if (component.downsampled_width != unsigned((expectedSize.width() + divisor - 1) / divisor)
                        || component.downsampled_height != unsigned((expectedSize.height() + divisor - 1) / divisor))
                        RawDecoder::fail(reinterpret_cast<j_common_ptr>(&codec));
                    frame->strides[i] = int(component.width_in_blocks) * DCTSIZE;
                    frame->planes[i].resize(qsizetype(frame->strides[i]) * groups * component.v_samp_factor * DCTSIZE);
                }
                while (codec.output_scanline < codec.output_height) {
                    JSAMPROW rows[3][16];
                    JSAMPARRAY planes[3] { rows[0], rows[1], rows[2] };
                    for (int i = 0; i < 3; ++i) {
                        const int count = codec.comp_info[i].v_samp_factor * DCTSIZE;
                        const int first = int(codec.output_scanline / 16) * count;
                        for (int row = 0; row < count; ++row)
                            rows[i][row] = reinterpret_cast<JSAMPROW>(
                                frame->planes[i].data() + qsizetype(first + row) * frame->strides[i]);
                    }
                    if (!jpeg_read_raw_data(&codec, planes, 16))
                        RawDecoder::fail(reinterpret_cast<j_common_ptr>(&codec));
                }
                jpeg_finish_decompress(&codec);
                if (error)
                    error->clear();
                return frame;
            }
        }
        frame->planes = {};
        frame->strides = {};
    }
    frame->rgb = decodeTrickplayFrame(bytes, crop, expectedSize, residentSheet, error);
    if (frame->rgb.isNull())
        return {};
    frame->size = frame->rgb.size();
    return frame;
}

QImage TrickplayTexture::image() const
{
    if (!planar())
        return rgb;
    // Software scene graphs and unsupported RHI formats use an RGB fallback.
    // Reconstruct the original 420 sampling, never an extra chroma downsample.
    QImage result(size, QImage::Format_RGB32);
    if (result.isNull())
        return {};
    const int cw = (size.width() + 1) / 2, ch = (size.height() + 1) / 2;
    const auto chroma = [&](int plane, int x, int y) {
        const float fx = (float(x) - .5f) * .5f, fy = (float(y) - .5f) * .5f;
        const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
        const float dx = fx - ix, dy = fy - iy;
        const auto sample = [&](int sx, int sy) {
            return float(uchar(
                planes[plane][qsizetype(std::clamp(sy, 0, ch - 1)) * strides[plane] + std::clamp(sx, 0, cw - 1)]));
        };
        return (sample(ix, iy) * (1 - dx) + sample(ix + 1, iy) * dx) * (1 - dy)
            + (sample(ix, iy + 1) * (1 - dx) + sample(ix + 1, iy + 1) * dx) * dy - 128.f;
    };
    const auto channel = [](float value) { return std::clamp(int(std::lround(value)), 0, 255); };
    for (int y = 0; y < size.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const float l = uchar(planes[0][qsizetype(y) * strides[0] + x]);
            const float cb = chroma(1, x, y), cr = chroma(2, x, y);
            row[x] = qRgb(
                channel(l + 1.402f * cr), channel(l - .344136286f * cb - .714136286f * cr), channel(l + 1.772f * cb));
        }
    }
    return result;
}

} // namespace Spool
