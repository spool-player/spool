#include "provider/ProviderExtensionData.h"
#include "TestMain.h"
#include "provider/ScriptBridge.h"

#include <QCoreApplication>
#include <QJSEngine>
#include <QLocale>
#include <QUrl>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>

using namespace Spool;
using namespace Spool::ProviderExtensionData;

namespace {
void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
QString failure(const std::function<void()>& call)
{
    try {
        call();
    } catch (const std::exception& error) {
        return QString::fromLatin1(error.what());
    }
    return {};
}
}

SPOOL_TEST_MAIN("provider-extension-data")
{
    QCoreApplication app(argc, argv);
    const QString key = QStringLiteral("278fca80-aaf9-4d32-8458-388836790234");
    const QVariant null = QVariant::fromValue(nullptr);
    const StorageInfo weak { MaximumBytes, false };
    const StorageInfo conditional { MaximumBytes, true };
    const StorageInfo small { 8, false };
    const auto read = preferenceResult("preferencesRead",
        { { "values",
              QVariantMap { { "audioLanguage", "en" }, { "subtitleLanguage", "de" }, { "audioMode", "Smart" },
                  { "subtitleMode", "OnlyForced" } } },
            { "writable", QVariantList { "audioLanguage", "subtitleMode" } } });
    require(read.value("values").toMap().value("audioLanguage") == "eng"
            && read.value("values").toMap().value("subtitleLanguage")
                == QLocale::languageToCode(QLocale::German, QLocale::ISO639Part2),
        "service two-letter languages normalize to the existing Qt ISO639-2 preference vocabulary");
    require(failure([&] { requireWritable({ { "audioMode", "Default" } }, read); }) == "preference_read_only",
        "an otherwise valid read-only preference cannot be written");
    require(failure([&] {
        preferenceArguments("preferencesWrite", { { "values", QVariantMap { { "subtitleMode", "forced" } } } });
    }) == "invalid_preferences",
        "provider-specific enum alternatives cannot leak into the normalized writable vocabulary");
    for (const QString field : { QStringLiteral("audioLanguage"), QStringLiteral("subtitleLanguage") }) {
        for (const QVariant& invalid : QVariantList { "en", "not-a-language", true, null }) {
            require(failure([&] {
                preferenceArguments("preferencesWrite", { { "values", QVariantMap { { field, invalid } } } });
            }) == "invalid_preferences",
                "writes use only canonical language preferences or an empty string");
        }
    }
    require(failure([&] {
        preferenceResult(
            "preferencesRead", { { "values", QVariantMap {} }, { "writable", QVariantList { "Policy" } } });
    }) == "invalid_extension_result",
        "unknown writable fields fail closed");
    require(failure([&] {
        preferenceArguments("preferencesWrite", { { "values", QVariantMap { { "Policy", QVariantMap {} } } } });
    }) == "invalid_preferences",
        "native service configuration is not an accepted preference patch");
    for (const QVariant& invalid : QVariantList { true, "64", 0, -1, 1.5 }) {
        require(failure([&] { storageInfo({ { "maxBytes", invalid }, { "conditionalWrites", false } }); })
                == "invalid_extension_result",
            "storage maximum must be a positive integral number");
    }
    require(storageInfo({ { "maxBytes", MaximumBytes * 2 }, { "conditionalWrites", false } }).maxBytes == MaximumBytes,
        "provider limits never enlarge the host document limit");
    require(
        failure([&] { storageInfo({ { "maxBytes", 64 }, { "conditionalWrites", 0 } }); }) == "invalid_extension_result",
        "CAS metadata is boolean, not truthy");
    for (const QString& invalid : QStringList { key.toUpper(), "{" + key + "}", "../data", key + "/", "" }) {
        require(failure([&] { storageArguments("dataRead", { { "key", invalid } }, &weak); }) == "invalid_data",
            "document keys cannot become paths or noncanonical aliases");
    }
    for (const QVariant& expected : QVariantList { null, QVariant(), "revision" }) {
        require(failure([&] {
            storageArguments(
                "dataWrite", { { "key", key }, { "value", null }, { "expectedRevision", expected } }, &weak);
        }) == "unsupported_condition",
            "even a null/undefined condition is rejected on weak storage");
        require(failure([&] {
            storageArguments("dataDelete", { { "key", key }, { "expectedRevision", expected } }, &weak);
        }) == "unsupported_condition",
            "weak deletes cannot pretend to enforce CAS either");
    }
    storageArguments("dataWrite", { { "key", key }, { "value", null }, { "expectedRevision", null } }, &conditional);
    const auto present = storageResult("dataRead", { { "found", true }, { "value", null } }, weak);
    const auto absent = storageResult("dataRead", { { "found", false } }, weak);
    require(present.value("found").toBool() && present.contains("value")
            && present.value("value").metaType().id() == QMetaType::Nullptr && !absent.contains("value"),
        "stored JSON null remains distinct from an absent document");
    for (const auto& invalid : QList<QVariantMap> { { { "found", true } }, { { "found", false }, { "value", null } },
             { { "found", 1 }, { "value", 2 } }, { { "found", true }, { "value", QVariant() } } }) {
        require(failure([&] { storageResult("dataRead", invalid, weak); }) == "invalid_extension_result",
            "ambiguous reads never become absence or an automatic overwrite");
    }
    require(failure([&] { storageResult("dataRead", { { "found", true }, { "value", null } }, conditional); })
            == "invalid_extension_result",
        "CAS reads must provide a revision for an existing document");
    require(storageResult("dataRead", { { "found", true }, { "value", null }, { "revision", "r1" } }, conditional)
                .value("revision")
            == "r1",
        "genuine revisions remain opaque");
    QVariant nested = null;
    for (int level = 0; level < 16; ++level)
        nested = QVariantList { nested };
    validateValue(nested, MaximumBytes, "invalid_data");
    nested = QVariantList { nested };
    require(failure([&] { validateValue(nested, MaximumBytes, "invalid_data"); }) == "invalid_data",
        "container nesting is bounded at sixteen");
    for (const QVariant& invalid : QVariantList { QVariant(), QUrl("https://example.test"), QByteArray("bytes"),
             std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
        require(failure([&] { validateValue(invalid, MaximumBytes, "invalid_data"); }) == "invalid_data",
            "only genuine finite JSON values cross the storage boundary");
    }
    validateValue(QString(MaximumBytes - 2, QLatin1Char('x')), MaximumBytes, "invalid_data");
    require(failure([&] { validateValue(QString(MaximumBytes - 1, QLatin1Char('x')), MaximumBytes, "invalid_data"); })
            == "data_too_large",
        "quotes count toward the compact JSON byte limit");
    require(failure([&] {
        storageArguments("dataWrite", { { "key", key }, { "value", QString::fromUtf8("éééé") } }, &small);
    }) == "data_too_large",
        "a provider's smaller limit counts UTF-8 bytes, not characters");
    require(failure([&] { storageResult("dataRead", { { "found", true }, { "value", "1234567" } }, small); })
            == "invalid_extension_result",
        "oversized remote documents are not silently accepted");
    QJSEngine engine;
    const auto wire
        = ownScriptValue(engine.evaluate("({found:true,value:null})"), 50000, 4 * 1024 * 1024, true).toMap();
    require(storageResult("dataRead", wire, weak).value("value").metaType().id() == QMetaType::Nullptr,
        "the actual JavaScript decoder preserves JSON null");
    const auto largeNumber
        = ownScriptValue(engine.evaluate("({found:true,value:1e100})"), 50000, 4 * 1024 * 1024, true).toMap();
    require(storageResult("dataRead", largeNumber, weak).value("value").toDouble() == 1e100,
        "finite JSON numbers are not confused with exact integer media ticks");
    require(failure([&] { ownScriptValue(engine.evaluate("1e100")); }) == "unsafe_number",
        "ordinary provider operations retain their safe-number boundary");
    return 0;
}
