#include "app/SettingsSyncDocument.h"

#include "TestMain.h"
#include "TestRequire.h"

#include <QJsonDocument>
#include <QSet>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace Spool::SettingsSyncDocument;

namespace {

using SpoolTests::require;

const QString LowNonce = QStringLiteral("00000000000000000000000000000000");
const QString HighNonce = QStringLiteral("ffffffffffffffffffffffffffffffff");

QVariantMap documentWith(QVariant value = true, QVariant clock = QStringLiteral("1"), QVariant nonce = LowNonce)
{
    return { { QStringLiteral("format"), 1 },
        { QStringLiteral("entries"),
            QVariantMap { { QStringLiteral("theme/accent"),
                QVariantMap { { QStringLiteral("value"), value }, { QStringLiteral("clock"), clock },
                    { QStringLiteral("nonce"), nonce } } } } } };
}

bool sameEntries(const Entries& left, const Entries& right)
{
    return encode(left) == encode(right);
}

void twoReplicaConvergence()
{
    const Entries baseline { { QStringLiteral("theme/accent"), { "blue", "100", LowNonce } } };
    Entries a = baseline;
    Entries b = baseline;
    a.insert(QStringLiteral("theme/accent"), makeEntry("red", maximumClock(a), LowNonce));
    b.insert(QStringLiteral("subtitles/mode"), makeEntry("OnlyForced", maximumClock(b), HighNonce));

    // Replacement-only storage loses A's acknowledged write when B writes a
    // stale whole document. A retains its replica and repairs on its next read.
    Entries server = a;
    a = merge(a, server);
    server = b;
    a = merge(a, server);
    server = a;
    b = merge(b, server);
    require(sameEntries(a, b), "different-key edits must converge after a stale replacement");
    require(a.value(QStringLiteral("theme/accent")).value == QVariant("red"),
        "acknowledged local maximum must survive a lower remote stamp");
    require(a.value(QStringLiteral("subtitles/mode")).value == QVariant("OnlyForced"),
        "merging must preserve another replica's independent key");
    require(sameEntries(merge(a, {}), a), "a missing remote entry must not erase retained state");
    require(sameEntries(merge(a, baseline), a), "a lower remote entry must not erase retained state");

    Entries sameA { { QStringLiteral("theme/accent"), makeEntry("red", "101", LowNonce) } };
    Entries sameB { { QStringLiteral("theme/accent"), makeEntry("green", "101", HighNonce) } };
    const Entries winnerA = merge(sameA, sameB);
    const Entries winnerB = merge(sameB, sameA);
    require(sameEntries(winnerA, winnerB), "same-counter ties must resolve identically in either order");
    require(winnerA.value(QStringLiteral("theme/accent")).value == QVariant("green"),
        "lexically greater nonce must win a same-counter tie");
    require(sameEntries(merge(winnerA, winnerA), winnerA), "merge must be idempotent");
}

void arbitraryPrecisionCounters()
{
    const QString huge(1000, u'9');
    require(nextClock(huge) == QStringLiteral("1") + QString(1000, u'0'),
        "carry must work beyond fixed-width integer counters");
    require(nextClock(QStringLiteral("000099")) == QStringLiteral("100"),
        "increment must canonicalize leading zeros without changing magnitude");
    require(compareClocks(QStringLiteral("00010"), QStringLiteral("10")) == 0,
        "counter equality must compare numeric magnitude");
    require(compareClocks(QStringLiteral("9007199254740993"), QStringLiteral("9007199254740992")) > 0,
        "counter comparison must not round through a floating-point number");
    require(compareClocks(huge, QStringLiteral("1") + QString(1000, u'0')) < 0,
        "counter comparison must compare digit count before lexical order");
    Entries remote { { QStringLiteral("future/key"), { QVariant(), huge, LowNonce } } };
    const auto parsed = decode(encode(remote));
    require(parsed.status == DecodeStatus::Valid && parsed.entries.value(QStringLiteral("future/key")).clock == huge,
        "large counters and JSON null must round-trip without precision loss");
    require(maximumClock(remote, nextClock(huge)) == nextClock(huge),
        "persisted counter must not regress when observed entries are older");
    const Entry provisional = makeEntry(false, maximumClock(remote, QStringLiteral("3")), HighNonce);
    require(compareClocks(provisional.clock, huge) > 0 && provisional.nonce == HighNonce,
        "provisional edit must be stamped above every bootstrap observation");
    for (const QString& bad : { QString(), QStringLiteral("-1"), QStringLiteral("1.5"), QStringLiteral("1e3"),
             QStringLiteral("+2"), QStringLiteral(" 2"), QString(QChar(0x0661)) }) {
        bool rejected = false;
        try {
            (void)nextClock(bad);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "invalid counter must not be allocated or silently normalized");
    }
}

void validationAndBounds()
{
    const QVariantMap good = documentWith();
    const QByteArray json = QJsonDocument::fromVariant(good).toJson(QJsonDocument::Compact);
    require(decodeJson(json, {}, json.size()).status == DecodeStatus::Valid,
        "exact byte limit must accept a bounded document");
    require(decodeJson(json, {}, json.size() - 1).status == DecodeStatus::Malformed,
        "one-byte overflow must reject the document");
    require(decode(good, {}, json.size()).status == DecodeStatus::Valid,
        "variant validation must account for exact compact JSON size");
    require(decode(good, {}, json.size() - 1).status == DecodeStatus::Malformed,
        "variant byte accounting must reject one-byte overflow");
    require(decodeJson("{broken").status == DecodeStatus::Malformed, "malformed JSON must reject");
    require(decode(QVariantList {}).status == DecodeStatus::Malformed, "array root must reject");
    require(decode(documentWith(true, 1)).status == DecodeStatus::Malformed,
        "numeric clock must reject rather than lose decimal precision");
    require(decode(documentWith(true, QStringLiteral("1"), HighNonce.toUpper())).status == DecodeStatus::Malformed,
        "uppercase nonce must reject");
    require(decode(documentWith(true, QStringLiteral("1"), QStringLiteral("abc"))).status == DecodeStatus::Malformed,
        "nonce must contain exactly 128 bits of lowercase hexadecimal");
    require(decode(documentWith(std::numeric_limits<double>::infinity())).status == DecodeStatus::Malformed,
        "nonfinite values must reject rather than become null");
    require(decode(documentWith(QByteArray("secret"))).status == DecodeStatus::Malformed,
        "non-JSON metatypes must not undergo implicit string conversion");
    require(decode(documentWith(QString(MaximumBytes, u'x'))).status == DecodeStatus::Malformed,
        "oversized document must reject before producing partial entries");

    QVariant nested = true;
    for (int i = 0; i < MaximumDepth - 3; ++i)
        nested = QVariantList { nested };
    require(decode(documentWith(nested)).status == DecodeStatus::Valid, "maximum total JSON depth must be accepted");
    nested = QVariantList { nested };
    require(decode(documentWith(nested)).status == DecodeStatus::Malformed,
        "depth overflow must reject the complete document");

    QVariantMap unknown = good;
    unknown.insert(QStringLiteral("format"), 2);
    require(decode(unknown).status == DecodeStatus::UnsupportedFormat,
        "future format must be distinguishable as read-only, not malformed");
    for (const QVariant& invalidFormat : { QVariant("1"), QVariant(0), QVariant(-1), QVariant(1.5), QVariant(true) }) {
        unknown.insert(QStringLiteral("format"), invalidFormat);
        require(decode(unknown).status == DecodeStatus::Malformed, "invalid format type or version must reject");
    }
    QVariantMap missingValue = good;
    QVariantMap entries = missingValue.value(QStringLiteral("entries")).toMap();
    QVariantMap row = entries.value(QStringLiteral("theme/accent")).toMap();
    row.remove(QStringLiteral("value"));
    entries.insert(QStringLiteral("theme/accent"), row);
    missingValue.insert(QStringLiteral("entries"), entries);
    require(decode(missingValue).status == DecodeStatus::Malformed,
        "absent value must be distinguished from a valid null value");

    bool rejected = false;
    try {
        (void)encode({ { QStringLiteral("future/key"), { QString(MaximumBytes, u'x'), "1", LowNonce } } });
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "encoding overflow must fail without silently truncating retained entries");
}

void filteringAndFutureValues()
{
    const QSet<QString> neverKeys { QStringLiteral("appearance/uiScalePercent"),
        QStringLiteral("playback/mpvConfigDirectory"), QStringLiteral("credentials/token"),
        QStringLiteral("certificates/trust"), QStringLiteral("device/id"), QStringLiteral("session/restore"),
        QStringLiteral("settingsSync/enabled") };
    const KeyFilter filter = [&](const QString& key) { return !neverKeys.contains(key); };
    Entries input;
    for (const QString& key : neverKeys)
        input.insert(key, { "must-not-leave-device", "99", HighNonce });
    const QVariantMap futureValue { { QStringLiteral("unknownEnum"), QStringLiteral("FutureMode") },
        { QStringLiteral("nested"), QVariantList { true, QVariant(), 7 } } };
    input.insert(QStringLiteral("future/moduleOption"), { futureValue, "100", LowNonce });
    const auto decoded = decode(encode(input), filter);
    require(decoded.status == DecodeStatus::Valid && decoded.entries.size() == 1,
        "host filter must remove known Never keys while retaining unknown future keys");
    require(decoded.entries.value(QStringLiteral("future/moduleOption")).value.toMap() == futureValue,
        "unknown future values must be preserved, not normalized to a local fallback");
    const QVariantMap filtered = encode(input, filter);
    require(sameEntries(decode(filtered).entries, decoded.entries),
        "outgoing serialization must strip sensitive keys independently of incoming filtering");
    require(sameEntries(merge(input, input, filter), decoded.entries),
        "merge filtering must not resurrect retained Never entries");

    const auto preserved = decodeJson(QJsonDocument::fromVariant(filtered).toJson(QJsonDocument::Compact));
    require(preserved.status == DecodeStatus::Valid
            && QJsonDocument::fromVariant(encode(preserved.entries)).toJson(QJsonDocument::Compact)
                == QJsonDocument::fromVariant(filtered).toJson(QJsonDocument::Compact),
        "opaque future JSON must survive transport and reserialization");
}

} // namespace

SPOOL_TEST_MAIN("settings-sync-document")
{
    twoReplicaConvergence();
    arbitraryPrecisionCounters();
    validationAndBounds();
    filteringAndFutureValues();
    return 0;
}
