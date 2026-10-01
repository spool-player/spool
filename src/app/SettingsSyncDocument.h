#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVariant>

#include <functional>

namespace Spool::SettingsSyncDocument {

inline constexpr qsizetype MaximumBytes = 64 * 1024;
inline constexpr int MaximumDepth = 16;

struct Entry {
    QVariant value;
    QString clock;
    QString nonce;
};
using Entries = QMap<QString, Entry>;
// Return false for known Never/sensitive keys; unknown future keys should return true.
using KeyFilter = std::function<bool(const QString&)>;

enum class DecodeStatus { Valid, UnsupportedFormat, Malformed };
struct DecodeResult {
    DecodeStatus status = DecodeStatus::Malformed;
    Entries entries;
    QString problem;
};

DecodeResult decode(const QVariant& document, const KeyFilter& retainKey = {}, qsizetype maximumBytes = MaximumBytes);
DecodeResult decodeJson(const QByteArray& json, const KeyFilter& retainKey = {}, qsizetype maximumBytes = MaximumBytes);
// Throws std::invalid_argument on invalid entries or bounds; never truncates data.
QVariantMap encode(const Entries& entries, const KeyFilter& retainKey = {}, qsizetype maximumBytes = MaximumBytes);

bool validClock(const QString& clock);
bool validNonce(const QString& nonce);
// Clock comparison is arbitrary precision; valid decimal strings are required.
int compareClocks(const QString& left, const QString& right);
int compareStamps(const Entry& left, const Entry& right);
// Equal stamps keep the retained entry. Missing/lower observations cannot erase it.
Entries merge(const Entries& retained, const Entries& observed, const KeyFilter& retainKey = {});
QString maximumClock(const Entries& entries, QString persistedCounter = QStringLiteral("0"));
QString nextClock(const QString& observedMaximum);
// Four independent system-generated 32-bit words: no UUID version/variant bits.
QString randomNonce();
// The caller persists the returned clock with its intent; inject a nonce for tests.
Entry makeEntry(QVariant value, const QString& observedMaximum, QString nonce = {});

} // namespace Spool::SettingsSyncDocument
