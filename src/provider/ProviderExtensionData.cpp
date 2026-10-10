#include "ProviderExtensionData.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Spool::ProviderExtensionData {
namespace {
    bool number(const QVariant& value)
    {
        switch (value.metaType().id()) {
        case QMetaType::Int:
        case QMetaType::UInt:
        case QMetaType::LongLong:
        case QMetaType::ULongLong:
        case QMetaType::Double:
        case QMetaType::Float:
            return std::isfinite(value.toDouble());
        default:
            return false;
        }
    }

    void fields(const QVariantMap& map, const QStringList& allowed, const char *error)
    {
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            if (!allowed.contains(it.key()))
                throw std::runtime_error(error);
        }
    }

    bool preferenceField(const QString& field)
    {
        return field == QStringLiteral("audioLanguage") || field == QStringLiteral("audioMode")
            || field == QStringLiteral("subtitleLanguage") || field == QStringLiteral("subtitleMode");
    }

    QVariantMap preferences(const QVariant& value, bool incoming, const char *error)
    {
        if (value.metaType().id() != QMetaType::QVariantMap)
            throw std::runtime_error(error);
        QVariantMap values = value.toMap();
        for (auto it = values.begin(); it != values.end(); ++it) {
            if (!preferenceField(it.key()) || it.value().metaType().id() != QMetaType::QString)
                throw std::runtime_error(error);
            const QString text = it.value().toString();
            if (it.key().endsWith(QStringLiteral("Language"))) {
                if (text.isEmpty())
                    continue;
                if ((text.size() != 3 && !(incoming && text.size() == 2)) || text != text.toLower())
                    throw std::runtime_error(error);
                const auto language = QLocale::codeToLanguage(QStringView(text));
                if (language == QLocale::AnyLanguage || language == QLocale::C)
                    throw std::runtime_error(error);
                const QString normalized = QLocale::languageToCode(language, QLocale::ISO639Part2);
                if (normalized.size() != 3)
                    throw std::runtime_error(error);
                it.value() = normalized;
            } else if (text != QStringLiteral("Default") && text != QStringLiteral("Smart")
                && !(it.key() == QStringLiteral("subtitleMode")
                    && (text == QStringLiteral("OnlyForced") || text == QStringLiteral("Always")
                        || text == QStringLiteral("None")))) {
                throw std::runtime_error(error);
            }
        }
        return values;
    }

    void validateJson(const QVariant& value, int depth, qsizetype& remaining, const char *error)
    {
        const auto consume = [&](qsizetype size) {
            if (size > remaining)
                throw std::runtime_error(QByteArray(error) == "invalid_data" ? "data_too_large" : error);
            remaining -= size;
        };
        const int type = value.metaType().id();
        if (type == QMetaType::QVariantMap) {
            if (depth >= 16)
                throw std::runtime_error(error);
            const auto map = value.toMap();
            consume(2);
            bool first = true;
            for (auto it = map.cbegin(); it != map.cend(); ++it) {
                consume(first ? 1 : 2); // Colon, plus comma after the first member.
                first = false;
                validateJson(it.key(), depth + 1, remaining, error);
                validateJson(it.value(), depth + 1, remaining, error);
            }
        } else if (type == QMetaType::QVariantList) {
            if (depth >= 16)
                throw std::runtime_error(error);
            const auto list = value.toList();
            consume(2);
            bool first = true;
            for (const auto& child : list) {
                if (!first)
                    consume(1);
                first = false;
                validateJson(child, depth + 1, remaining, error);
            }
        } else if (type == QMetaType::Nullptr) {
            consume(4);
        } else if (type == QMetaType::Bool) {
            consume(value.toBool() ? 4 : 5);
        } else if (type == QMetaType::QString) {
            // A lower bound keeps the subsequent single JSON serialization bounded.
            consume(value.toString().size() + 2);
        } else if (number(value)) {
            consume(1);
        } else {
            // Undefined, byte arrays, dates and arbitrary native values are not JSON null.
            throw std::runtime_error(error);
        }
    }

    void revision(const QVariantMap& map, const QString& key, bool allowNull, const char *error)
    {
        if (!map.contains(key))
            return;
        const auto value = map.value(key);
        if (allowNull && value.metaType().id() == QMetaType::Nullptr)
            return;
        if (value.metaType().id() != QMetaType::QString || value.toString().size() > MaximumBytes)
            throw std::runtime_error(error);
    }
} // namespace

StorageInfo storageInfo(const QVariantMap& result)
{
    const auto maximum = result.value(QStringLiteral("maxBytes"));
    const double size = maximum.toDouble();
    if (!number(maximum) || size < 1 || size > std::numeric_limits<int>::max() || std::floor(size) != size
        || result.value(QStringLiteral("conditionalWrites")).metaType().id() != QMetaType::Bool)
        throw std::runtime_error("invalid_extension_result");
    return { std::min(static_cast<int>(size), MaximumBytes),
        result.value(QStringLiteral("conditionalWrites")).toBool() };
}

void validateValue(const QVariant& value, int maxBytes, const char *error)
{
    const int maximum = std::min(maxBytes, MaximumBytes);
    qsizetype remaining = maximum;
    validateJson(value, 0, remaining, error);
    // Wrap scalars too, then exclude the array delimiters. Serialize once, not
    // once per leaf in a potentially wide application document.
    if (QJsonDocument(QJsonArray { QJsonValue::fromVariant(value) }).toJson(QJsonDocument::Compact).size() - 2
        > maximum)
        throw std::runtime_error(QByteArray(error) == "invalid_data" ? "data_too_large" : error);
}

QVariantMap preferenceArguments(const QString& operation, const QVariantMap& arguments)
{
    fields(arguments,
        operation == QStringLiteral("preferencesWrite") ? QStringList { QStringLiteral("values") } : QStringList {},
        "invalid_preferences");
    if (operation == QStringLiteral("preferencesWrite"))
        return { { QStringLiteral("values"),
            preferences(arguments.value(QStringLiteral("values")), false, "invalid_preferences") } };
    return {};
}

QVariantMap preferenceResult(const QString& operation, const QVariantMap& result)
{
    if (operation == QStringLiteral("preferencesWrite")) {
        if (!result.isEmpty())
            throw std::runtime_error("invalid_extension_result");
        return {};
    }
    const auto values = preferences(result.value(QStringLiteral("values")), true, "invalid_extension_result");
    const auto writable = result.value(QStringLiteral("writable"));
    if (writable.metaType().id() != QMetaType::QVariantList)
        throw std::runtime_error("invalid_extension_result");
    QSet<QString> seen;
    for (const auto& field : writable.toList()) {
        if (field.metaType().id() != QMetaType::QString || !preferenceField(field.toString())
            || seen.contains(field.toString()))
            throw std::runtime_error("invalid_extension_result");
        seen.insert(field.toString());
    }
    return { { QStringLiteral("values"), values }, { QStringLiteral("writable"), writable } };
}

void requireWritable(const QVariantMap& values, const QVariantMap& preferences)
{
    const auto writable = preferences.value(QStringLiteral("writable")).toList();
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (!writable.contains(it.key()))
            throw std::runtime_error("preference_read_only");
    }
}

void storageArguments(const QString& operation, const QVariantMap& arguments, const StorageInfo *info)
{
    if (operation == QStringLiteral("dataInfo")) {
        fields(arguments, {}, "invalid_data");
        return;
    }
    const bool write = operation == QStringLiteral("dataWrite");
    const bool mutation = write || operation == QStringLiteral("dataDelete");
    QStringList allowed { QStringLiteral("key") };
    if (write)
        allowed.append(QStringLiteral("value"));
    if (mutation)
        allowed.append(QStringLiteral("expectedRevision"));
    fields(arguments, allowed, "invalid_data");
    static const QRegularExpression uuid(
        QStringLiteral("\\A[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\z"));
    const auto key = arguments.value(QStringLiteral("key"));
    if (key.metaType().id() != QMetaType::QString || !uuid.match(key.toString()).hasMatch())
        throw std::runtime_error("invalid_data");
    if (mutation && info && arguments.contains(QStringLiteral("expectedRevision")) && !info->conditionalWrites)
        throw std::runtime_error("unsupported_condition");
    if (info)
        revision(arguments, QStringLiteral("expectedRevision"), true, "invalid_data");
    if (write) {
        if (!arguments.contains(QStringLiteral("value")))
            throw std::runtime_error("invalid_data");
        validateValue(arguments.value(QStringLiteral("value")), info ? info->maxBytes : MaximumBytes, "invalid_data");
    }
}

QVariantMap storageResult(const QString& operation, const QVariantMap& result, const StorageInfo& info)
{
    if (operation == QStringLiteral("dataInfo")) {
        const auto decoded = storageInfo(result);
        return { { QStringLiteral("maxBytes"), decoded.maxBytes },
            { QStringLiteral("conditionalWrites"), decoded.conditionalWrites } };
    }
    if (operation == QStringLiteral("dataDelete")) {
        if (!result.isEmpty())
            throw std::runtime_error("invalid_extension_result");
        return {};
    }
    revision(result, QStringLiteral("revision"), false, "invalid_extension_result");
    QVariantMap decoded;
    if (result.contains(QStringLiteral("revision"))) {
        decoded.insert(QStringLiteral("revision"), result.value(QStringLiteral("revision")));
    }
    if (operation == QStringLiteral("dataRead")) {
        const auto found = result.value(QStringLiteral("found"));
        if (found.metaType().id() != QMetaType::Bool || found.toBool() != result.contains(QStringLiteral("value"))
            || (!found.toBool() && result.contains(QStringLiteral("revision")))
            || (found.toBool() && info.conditionalWrites && !result.contains(QStringLiteral("revision"))))
            throw std::runtime_error("invalid_extension_result");
        decoded.insert(QStringLiteral("found"), found);
        if (found.toBool()) {
            validateValue(result.value(QStringLiteral("value")), info.maxBytes, "invalid_extension_result");
            decoded.insert(QStringLiteral("value"), result.value(QStringLiteral("value")));
        }
    } else if (info.conditionalWrites && !result.contains(QStringLiteral("revision"))) {
        throw std::runtime_error("invalid_extension_result");
    }
    return decoded;
}

} // namespace Spool::ProviderExtensionData
