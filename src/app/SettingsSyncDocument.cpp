#include "SettingsSyncDocument.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRandomGenerator>
#include <QStringView>

#include <cmath>
#include <stdexcept>
#include <utility>

namespace Spool::SettingsSyncDocument {
namespace {

    bool numericType(int type)
    {
        return type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::LongLong
            || type == QMetaType::ULongLong || type == QMetaType::Double || type == QMetaType::Float;
    }

    // Validate types/depth and account for exact compact JSON bytes before creating
    // the document. Stop as soon as the budget is exceeded, including large lists.
    bool consumeJson(const QVariant& value, int depth, qsizetype& remaining)
    {
        if (depth > MaximumDepth || remaining < 0)
            return false;
        const int type = value.metaType().id();
        if (type == QMetaType::QVariantMap) {
            remaining -= 2;
            const QVariantMap map = value.toMap();
            bool first = true;
            for (auto it = map.cbegin(); it != map.cend(); ++it) {
                remaining -= first ? 1 : 2; // colon, and comma after the first member
                first = false;
                if (!consumeJson(it.key(), depth + 1, remaining) || !consumeJson(it.value(), depth + 1, remaining))
                    return false;
            }
        } else if (type == QMetaType::QVariantList) {
            remaining -= 2;
            const QVariantList list = value.toList();
            bool first = true;
            for (const QVariant& item : list) {
                if (!first)
                    --remaining;
                first = false;
                if (!consumeJson(item, depth + 1, remaining))
                    return false;
            }
        } else if (!value.isValid() || type == QMetaType::Nullptr) {
            remaining -= 4;
        } else if (type == QMetaType::QString) {
            // Each UTF-16 code unit takes at least one JSON byte. This precheck
            // avoids allocating a JSON encoding for an already oversized string.
            if (value.toString().size() > remaining)
                return false;
            remaining
                -= QJsonDocument(QJsonArray { QJsonValue(value.toString()) }).toJson(QJsonDocument::Compact).size() - 2;
        } else if (type == QMetaType::Bool || numericType(type)) {
            if (numericType(type) && !std::isfinite(value.toDouble()))
                return false;
            remaining
                -= QJsonDocument(QJsonArray { QJsonValue::fromVariant(value) }).toJson(QJsonDocument::Compact).size()
                - 2;
        } else {
            // QVariant's implicit conversions must not silently turn dates, paths,
            // bytes or custom metatypes into uploadable JSON strings.
            return false;
        }
        return remaining >= 0;
    }

    bool boundedJson(const QVariant& value, qsizetype maximumBytes)
    {
        if (maximumBytes <= 0)
            return false;
        qsizetype remaining = qMin(maximumBytes, MaximumBytes);
        return consumeJson(value, 0, remaining);
    }

    QStringView significantDigits(const QString& clock)
    {
        qsizetype first = 0;
        while (first + 1 < clock.size() && clock.at(first) == u'0')
            ++first;
        return QStringView(clock).mid(first);
    }

    DecodeResult malformed(const QString& problem)
    {
        return { DecodeStatus::Malformed, {}, problem };
    }

    bool keep(const KeyFilter& filter, const QString& key)
    {
        return !filter || filter(key);
    }

} // namespace

bool validClock(const QString& clock)
{
    if (clock.isEmpty())
        return false;
    for (const QChar digit : clock) {
        if (digit < u'0' || digit > u'9')
            return false;
    }
    return true;
}

bool validNonce(const QString& nonce)
{
    if (nonce.size() != 32)
        return false;
    for (const QChar digit : nonce) {
        if (!((digit >= u'0' && digit <= u'9') || (digit >= u'a' && digit <= u'f')))
            return false;
    }
    return true;
}

DecodeResult decode(const QVariant& document, const KeyFilter& retainKey, qsizetype maximumBytes)
{
    if (!boundedJson(document, maximumBytes))
        return malformed(QStringLiteral("Settings sync data exceeds its bounds or is not valid JSON."));
    if (document.metaType().id() != QMetaType::QVariantMap)
        return malformed(QStringLiteral("Settings sync data must be an object."));
    const QVariantMap root = document.toMap();
    const QVariant format = root.value(QStringLiteral("format"));
    if (!numericType(format.metaType().id()) || format.toDouble() < 1
        || std::floor(format.toDouble()) != format.toDouble())
        return malformed(QStringLiteral("Settings sync data has an invalid format."));
    if (format.toDouble() != 1)
        return { DecodeStatus::UnsupportedFormat, {},
            QStringLiteral("Update Spool to read this settings sync format.") };
    const QVariant rawEntries = root.value(QStringLiteral("entries"));
    if (rawEntries.metaType().id() != QMetaType::QVariantMap)
        return malformed(QStringLiteral("Settings sync entries must be an object."));
    Entries entries;
    const QVariantMap rows = rawEntries.toMap();
    for (auto it = rows.cbegin(); it != rows.cend(); ++it) {
        if (it.key().isEmpty() || it.value().metaType().id() != QMetaType::QVariantMap)
            return malformed(QStringLiteral("Settings sync contains an invalid entry."));
        const QVariantMap row = it.value().toMap();
        const QVariant clock = row.value(QStringLiteral("clock"));
        const QVariant nonce = row.value(QStringLiteral("nonce"));
        if (!row.contains(QStringLiteral("value")) || clock.metaType().id() != QMetaType::QString
            || !validClock(clock.toString()) || nonce.metaType().id() != QMetaType::QString
            || !validNonce(nonce.toString()))
            return malformed(QStringLiteral("Settings sync contains an invalid value or stamp."));
        if (keep(retainKey, it.key()))
            entries.insert(it.key(), { row.value(QStringLiteral("value")), clock.toString(), nonce.toString() });
    }
    return { DecodeStatus::Valid, std::move(entries), {} };
}

DecodeResult decodeJson(const QByteArray& json, const KeyFilter& retainKey, qsizetype maximumBytes)
{
    if (maximumBytes <= 0 || json.size() > qMin(maximumBytes, MaximumBytes))
        return malformed(QStringLiteral("Settings sync data exceeds its size limit."));
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError)
        return malformed(QStringLiteral("Settings sync data is malformed JSON."));
    return decode(document.toVariant(), retainKey, maximumBytes);
}

QVariantMap encode(const Entries& entries, const KeyFilter& retainKey, qsizetype maximumBytes)
{
    QVariantMap rows;
    // Include the fixed envelope before copying any entries into the result.
    constexpr qsizetype envelopeBytes = sizeof("{\"format\":1,\"entries\":{}}") - 1;
    qsizetype remaining = qMin(maximumBytes, MaximumBytes) - envelopeBytes;
    if (remaining < 0)
        throw std::invalid_argument("Settings sync document exceeds its size limit");
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        if (!keep(retainKey, it.key()))
            continue;
        const Entry& entry = it.value();
        if (it.key().isEmpty() || !validClock(entry.clock) || !validNonce(entry.nonce))
            throw std::invalid_argument("Invalid settings sync entry stamp");
        const QVariantMap row { { QStringLiteral("value"), entry.value }, { QStringLiteral("clock"), entry.clock },
            { QStringLiteral("nonce"), entry.nonce } };
        remaining -= rows.isEmpty() ? 1 : 2; // colon, then comma between entries
        if (!consumeJson(it.key(), 2, remaining) || !consumeJson(row, 2, remaining))
            throw std::invalid_argument("Settings sync document exceeds bounds or contains non-JSON values");
        rows.insert(it.key(), row);
    }
    return { { QStringLiteral("format"), 1 }, { QStringLiteral("entries"), rows } };
}

int compareClocks(const QString& left, const QString& right)
{
    if (!validClock(left) || !validClock(right))
        throw std::invalid_argument("Invalid settings sync clock");
    const QStringView a = significantDigits(left);
    const QStringView b = significantDigits(right);
    if (a.size() != b.size())
        return a.size() < b.size() ? -1 : 1;
    const int order = a.compare(b);
    return order < 0 ? -1 : order > 0 ? 1 : 0;
}

int compareStamps(const Entry& left, const Entry& right)
{
    if (!validNonce(left.nonce) || !validNonce(right.nonce))
        throw std::invalid_argument("Invalid settings sync nonce");
    const int clocks = compareClocks(left.clock, right.clock);
    if (clocks != 0)
        return clocks;
    const int order = left.nonce.compare(right.nonce);
    return order < 0 ? -1 : order > 0 ? 1 : 0;
}

Entries merge(const Entries& retained, const Entries& observed, const KeyFilter& retainKey)
{
    Entries result;
    const auto add = [&](const Entries& source) {
        for (auto it = source.cbegin(); it != source.cend(); ++it) {
            if (!keep(retainKey, it.key()))
                continue;
            const auto existing = result.constFind(it.key());
            if (existing == result.cend() || compareStamps(it.value(), existing.value()) > 0)
                result.insert(it.key(), it.value());
        }
    };
    add(retained);
    add(observed);
    return result;
}

QString maximumClock(const Entries& entries, QString persistedCounter)
{
    if (!validClock(persistedCounter))
        throw std::invalid_argument("Invalid persisted settings sync counter");
    for (const Entry& entry : entries) {
        if (compareClocks(entry.clock, persistedCounter) > 0)
            persistedCounter = entry.clock;
    }
    return significantDigits(persistedCounter).toString();
}

QString nextClock(const QString& observedMaximum)
{
    if (!validClock(observedMaximum))
        throw std::invalid_argument("Invalid settings sync clock");
    QString result = significantDigits(observedMaximum).toString();
    for (qsizetype i = result.size(); i > 0; --i) {
        if (result.at(i - 1) != u'9') {
            result[i - 1] = QChar(result.at(i - 1).unicode() + 1);
            return result;
        }
        result[i - 1] = u'0';
    }
    result.prepend(u'1');
    return result;
}

QString randomNonce()
{
    static constexpr char digits[] = "0123456789abcdef";
    QString nonce(32, u'0');
    for (qsizetype word = 0; word < 4; ++word) {
        quint32 random = QRandomGenerator::system()->generate();
        for (qsizetype digit = 0; digit < 8; ++digit) {
            nonce[word * 8 + digit] = QLatin1Char(digits[random & 0xf]);
            random >>= 4;
        }
    }
    return nonce;
}

Entry makeEntry(QVariant value, const QString& observedMaximum, QString nonce)
{
    if (nonce.isEmpty())
        nonce = randomNonce();
    if (!validNonce(nonce))
        throw std::invalid_argument("Invalid settings sync nonce");
    return { std::move(value), nextClock(observedMaximum), std::move(nonce) };
}

} // namespace Spool::SettingsSyncDocument
