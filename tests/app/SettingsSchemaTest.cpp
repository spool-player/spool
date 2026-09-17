#include "app/SettingsSchema.h"
#include "app/LocalizationManager.h"

#include "TestMain.h"

#include <QCoreApplication>
#include <QHash>
#include <QLocale>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>

using namespace JellyfinNative;

namespace {

QString keyString(const SettingSpec& spec)
{
    return QString::fromLatin1(spec.key);
}

void require(bool condition, const QString& message)
{
    if (condition)
        return;
    std::cerr << message.toStdString() << '\n';
    std::exit(EXIT_FAILURE);
}

const SettingSpec& requiredSpec(const QString& key)
{
    const SettingSpec *spec = findSettingSpec(key);
    require(spec != nullptr, QStringLiteral("missing setting spec for %1").arg(key));
    return *spec;
}

QSet<QString> stringSet(const QStringList& values)
{
    QSet<QString> result;
    result.reserve(values.size());
    for (const QString& value : values)
        result.insert(value);
    return result;
}

QStringList choiceValues(const SettingSpec& spec)
{
    QStringList values;
    values.reserve(spec.choiceCount);
    for (qsizetype i = 0; i < spec.choiceCount; ++i)
        values.push_back(QString::fromLatin1(spec.choices[i].value));
    return values;
}

QString choiceLabel(const SettingSpec& spec, const QString& value)
{
    for (qsizetype i = 0; i < spec.choiceCount; ++i) {
        if (value == QString::fromLatin1(spec.choices[i].value))
            return QString::fromLatin1(spec.choices[i].label);
    }
    return {};
}

QVariantMap schemaRow(const QString& key)
{
    for (const QVariant& item : settingSchemaModel()) {
        const QVariantMap row = item.toMap();
        if (row.value(QStringLiteral("key")).toString() == key)
            return row;
    }
    return {};
}

QHash<QString, QString> choicesByLabelFromRow(const QVariantMap& row)
{
    const QVariantList values = row.value(QStringLiteral("choiceValues")).toList();
    const QVariantList labels = row.value(QStringLiteral("choiceLabels")).toList();
    require(values.size() == labels.size(), QStringLiteral("schema row choice values and labels diverged"));

    QHash<QString, QString> result;
    result.reserve(values.size());
    for (qsizetype i = 0; i < values.size(); ++i)
        result.insert(values.at(i).toString(), labels.at(i).toString());
    return result;
}

void requiredPersistedKeysArePresentExactlyOnce()
{
    const QStringList expectedKeys {
        QStringLiteral("appearance/uiScalePercent"),
        QStringLiteral("remote/showCastButton"),
        QStringLiteral("remote/acceptCommands"),
        QStringLiteral("artwork/format"),
        QStringLiteral("artwork/webpQuality"),
        QStringLiteral("artwork/jpegQuality"),
        QStringLiteral("audio/trackMode"),
        QStringLiteral("playback/rememberSeriesAudioTrack"),
        QStringLiteral("settings/nightMode"),
        QStringLiteral("playback/maxStreamingHeight"),
        QStringLiteral("playback/manualStreamingBitrate"),
        QStringLiteral("playback/maxStreamingBitrateMbps"),
        QStringLiteral("playback/unlimitedLocalBitrate"),
        QStringLiteral("playback/preferRemux"),
        QStringLiteral("playback/forwardCacheSizeMiB"),
        QStringLiteral("playback/showVolumeSlider"),
        QStringLiteral("playback/controlFadeDelaySeconds"),
        QStringLiteral("settings/audioDelayMs"),
        QStringLiteral("settings/audioOutputMode"),
        QStringLiteral("subtitles/language"),
        QStringLiteral("playback/mpvConfigMode"),
        QStringLiteral("playback/mpvConfigDirectory"),
        QStringLiteral("subtitles/mode"),
        QStringLiteral("subtitles/styling"),
        QStringLiteral("subtitles/scalePercent"),
        QStringLiteral("subtitles/overrideTextColor"),
        QStringLiteral("subtitles/alwaysOverridePositionAndSize"),
        QStringLiteral("subtitles/bitmapSharpnessPercent"),
        QStringLiteral("subtitles/recolorImageSubtitles"),
        QStringLiteral("subtitles/bitmapShadowEnabled"),
        QStringLiteral("subtitles/bitmapShadowCoreSize"),
        QStringLiteral("subtitles/bitmapShadowCoreGrow"),
        QStringLiteral("subtitles/bitmapShadowCoreOpacityPercent"),
        QStringLiteral("subtitles/bitmapShadowSpreadEnabled"),
        QStringLiteral("subtitles/bitmapShadowSpreadSize"),
        QStringLiteral("subtitles/bitmapShadowSpreadGrow"),
        QStringLiteral("subtitles/bitmapShadowSpreadX"),
        QStringLiteral("subtitles/bitmapShadowSpreadY"),
        QStringLiteral("subtitles/bitmapShadowSpreadOpacityPercent"),
        QStringLiteral("subtitles/bitmapShadowDither"),
        QStringLiteral("subtitles/allowInBlackBars"),
        QStringLiteral("subtitles/textWeight"),
        QStringLiteral("subtitles/font"),
        QStringLiteral("subtitles/textColor"),
        QStringLiteral("subtitles/dropShadow"),
        QStringLiteral("subtitles/textBackground"),
        QStringLiteral("subtitles/verticalPositionPercent"),
        QStringLiteral("subtitles/hdrBrightnessPercent"),
        QStringLiteral("settings/toneMappingVisualization"),
        QStringLiteral("input/redButton"),
        QStringLiteral("input/greenButton"),
        QStringLiteral("input/yellowButton"),
        QStringLiteral("input/blueButton"),
        QStringLiteral("updates/automatic"),
        QStringLiteral("playback/videoOutput"),
        QStringLiteral("playback/renderQuality"),
        QStringLiteral("playback/autoAdjustQuality"),
        QStringLiteral("playback/hdrOutput"),
        QStringLiteral("playback/graphicsApi"),
        QStringLiteral("playback/hdrPeakNits"),
    };

    QHash<QString, int> counts;
    counts.reserve(expectedKeys.size());
    for (const SettingSpec& spec : settingSpecs()) {
        if (!spec.persisted)
            continue;
        const QString key = keyString(spec);
        counts[key] += 1;
    }

    for (const QString& key : expectedKeys) {
        require(counts.value(key) == 1,
            QStringLiteral("persisted setting key %1 appeared %2 times").arg(key).arg(counts.value(key)));
    }
}

void audioOutputChoicesMatchPlatform()
{
    const SettingSpec& audioOutput = requiredSpec(QStringLiteral("settings/audioOutputMode"));
#ifdef JELLYFIN_NATIVE_WEBOS
    const QStringList expectedChoices { QStringLiteral("alsa"), QStringLiteral("starfish-pcm") };
    const QString expectedDefault = QStringLiteral("alsa");
    const QString unknownFallback = QStringLiteral("alsa");
    require(
        normalizedSettingValue(audioOutput, QStringLiteral("starfish")).toString() == QStringLiteral("starfish-pcm"),
        QStringLiteral("legacy Starfish output did not normalize to starfish-pcm"));
#elif defined(Q_OS_LINUX)
    const QStringList expectedChoices { QStringLiteral("auto"), QStringLiteral("pipewire"), QStringLiteral("pulse"),
        QStringLiteral("alsa") };
    const QString expectedDefault = QStringLiteral("auto");
    const QString unknownFallback = QStringLiteral("auto");
#elif defined(Q_OS_WIN)
    const QStringList expectedChoices { QStringLiteral("auto"), QStringLiteral("wasapi") };
    const QString expectedDefault = QStringLiteral("auto");
    const QString unknownFallback = QStringLiteral("auto");
#elif defined(Q_OS_MACOS)
    const QStringList expectedChoices { QStringLiteral("auto"), QStringLiteral("coreaudio") };
    const QString expectedDefault = QStringLiteral("auto");
    const QString unknownFallback = QStringLiteral("auto");
#else
    const QStringList expectedChoices { QStringLiteral("auto") };
    const QString expectedDefault = QStringLiteral("auto");
    const QString unknownFallback = QStringLiteral("auto");
#endif

    require(choiceValues(audioOutput) == expectedChoices,
        QStringLiteral("audio output choices do not match this platform"));
    require(settingDefaultValue(audioOutput).toString() == expectedDefault,
        QStringLiteral("audio output default does not match this platform"));
    for (const QString& choice : expectedChoices) {
        require(normalizedSettingValue(audioOutput, choice).toString() == choice,
            QStringLiteral("audio output choice %1 was not preserved").arg(choice));
    }
    require(normalizedSettingValue(audioOutput, QStringLiteral("unexpected")).toString() == unknownFallback,
        QStringLiteral("unknown audio output did not use the platform default"));
#ifndef JELLYFIN_NATIVE_WEBOS
    require(!expectedChoices.contains(QStringLiteral("starfish-pcm")),
        QStringLiteral("desktop audio choices must not expose Starfish"));
#endif
}

void resolutionAndBitrateAreSettledSeparately()
{
    // Two ceilings, two questions. A viewer sparing a slow decoder wants a
    // smaller picture at the bitrate they have; a viewer on a metered
    // connection wants fewer bits at the size they have. Neither answer
    // should be buried in the other, and the resolution limit must not be
    // conditioned on the manual-bitrate toggle the way the slider is.
    const SettingSpec& height = requiredSpec(QStringLiteral("playback/maxStreamingHeight"));
    const SettingSpec& bitrateLimit = requiredSpec(QStringLiteral("playback/maxStreamingBitrateMbps"));
    require(
        height.type == SettingType::Select, QStringLiteral("the resolution limit should be a choice, not megabits"));
    require(
        height.group == bitrateLimit.group, QStringLiteral("both ceilings belong beside each other under Streaming"));
    require(settingDefaultValue(height).toInt() == 0,
        QStringLiteral("the resolution limit should default to leaving the source alone"));
    require(height.target == SettingTarget::MaxStreamingHeight,
        QStringLiteral("the resolution limit must reach the device profile, not the bitrate ceiling"));
    require(height.dependsOnKey == nullptr || QString::fromLatin1(height.dependsOnKey).isEmpty(),
        QStringLiteral("capping resolution must not require opting into a manual bitrate first"));
    require(bitrateLimit.target == SettingTarget::MaxStreamingBitrate,
        QStringLiteral("the bitrate ceiling should be unchanged by the resolution one"));

    bool sawSource = false;
    bool saw480 = false;
    for (qsizetype index = 0; index < height.choiceCount; ++index) {
        const QString value = QString::fromLatin1(height.choices[index].value);
        sawSource |= value == QStringLiteral("0");
        saw480 |= value == QStringLiteral("480");
    }
    require(sawSource, QStringLiteral("the resolution limit should offer leaving the source alone"));
    require(saw480, QStringLiteral("the resolution limit should offer the rungs the overlay offers"));
}

void normalizersPreservePersistedValueSemantics()
{
    const SettingSpec& bitrate = requiredSpec(QStringLiteral("playback/maxStreamingBitrateMbps"));
    require(normalizedSettingValue(bitrate, QStringLiteral("4")).toInt() == 5,
        QStringLiteral("streaming bitrate below the floor was not clamped"));
    require(normalizedSettingValue(bitrate, QStringLiteral("1001")).toInt() == 1000,
        QStringLiteral("streaming bitrate above the ceiling was not clamped"));
    require(serializedSettingValue(bitrate, QStringLiteral("42")) == QStringLiteral("42"),
        QStringLiteral("in-range streaming bitrate was not serialized unchanged"));
    const SettingSpec& forwardCache = requiredSpec(QStringLiteral("playback/forwardCacheSizeMiB"));
    require(forwardCache.type == SettingType::Slider && forwardCache.minimum == 16 && forwardCache.maximum == 2048,
        QStringLiteral("forward cache should span 16-2048 MB"));
    require(settingDefaultValue(forwardCache).toInt() == 32, QStringLiteral("forward cache should default to 32 MB"));
    require(normalizedSettingValue(forwardCache, QStringLiteral("256")).toInt() == 256,
        QStringLiteral("valid forward cache size was not preserved"));
    require(normalizedSettingValue(forwardCache, QStringLiteral("300")).toInt() == 256,
        QStringLiteral("forward cache did not snap to the nearest power of two"));
    require(normalizedSettingValue(forwardCache, QStringLiteral("1")).toInt() == 16,
        QStringLiteral("forward cache below 16 MB was not clamped"));
    require(normalizedSettingValue(forwardCache, QStringLiteral("5000")).toInt() == 2048,
        QStringLiteral("forward cache above 2048 MB was not clamped"));
    const SettingSpec& controlFade = requiredSpec(QStringLiteral("playback/controlFadeDelaySeconds"));
    require(controlFade.type == SettingType::Slider && controlFade.minimum == 1 && controlFade.maximum == 10
            && controlFade.step == 1,
        QStringLiteral("playback control fade delay should span 1-10 seconds in one-second steps"));
    require(QString::fromLatin1(controlFade.title) == QStringLiteral("Hide player controls after"),
        QStringLiteral("playback control fade delay title was not preserved"));
    require(settingDefaultValue(controlFade).toInt() == 4,
        QStringLiteral("playback controls should keep the existing four-second fade delay by default"));
    require(normalizedSettingValue(controlFade, QStringLiteral("11")).toInt() == 10,
        QStringLiteral("playback control fade delay above 10 seconds was not clamped"));
    const SettingSpec& uiScale = requiredSpec(QStringLiteral("appearance/uiScalePercent"));
    require(normalizedSettingValue(uiScale, QStringLiteral("45")).toInt() == 50,
        QStringLiteral("UI scale below the floor was not clamped"));
    require(normalizedSettingValue(uiScale, QStringLiteral("65")).toInt() == 65,
        QStringLiteral("UI scale above the floor was clamped"));
    require(normalizedSettingValue(uiScale, QStringLiteral("181")).toInt() == 180,
        QStringLiteral("UI scale above the ceiling was not clamped"));
    require(serializedSettingValue(uiScale, QStringLiteral("115")) == QStringLiteral("115"),
        QStringLiteral("in-range UI scale was not serialized as a percentage"));

    const SettingSpec& nightMode = requiredSpec(QStringLiteral("settings/nightMode"));
    require(normalizedSettingValue(nightMode, true).toBool(),
        QStringLiteral("boolean QVariant true did not normalize to true"));
    require(normalizedSettingValue(nightMode, QStringLiteral(" YES ")).toBool(),
        QStringLiteral("yes boolean text did not normalize to true"));
    require(normalizedSettingValue(nightMode, QStringLiteral("1")).toBool(),
        QStringLiteral("1 boolean text did not normalize to true"));
    require(!normalizedSettingValue(nightMode, QStringLiteral("0")).toBool(),
        QStringLiteral("0 boolean text did not normalize to false"));
    require(serializedSettingValue(nightMode, QStringLiteral("yes")) == QStringLiteral("true"),
        QStringLiteral("truthy boolean text did not serialize to true"));

    const SettingSpec& textColor = requiredSpec(QStringLiteral("subtitles/textColor"));
    require(normalizedSettingValue(textColor, QStringLiteral(" #A0b1C2 ")).toString() == QStringLiteral("#a0b1c2"),
        QStringLiteral("valid subtitle colour was not trimmed and lower-cased"));
    require(normalizedSettingValue(textColor, QStringLiteral("blue")).toString() == QStringLiteral("#ffffff"),
        QStringLiteral("invalid subtitle colour did not fall back to white"));

    const SettingSpec& dropShadow = requiredSpec(QStringLiteral("subtitles/dropShadow"));
    require(normalizedSettingValue(dropShadow, QStringLiteral("uniform")).toString() == QStringLiteral("uniform"),
        QStringLiteral("valid subtitle drop-shadow choice was not preserved"));
    require(normalizedSettingValue(dropShadow, QStringLiteral("outer-glow")).toString().isEmpty(),
        QStringLiteral("invalid subtitle drop-shadow choice did not fall back to the default choice"));
}

void subtitleGeometryOverrideMatchesSchemaContract()
{
    const SettingSpec& override = requiredSpec(QStringLiteral("subtitles/alwaysOverridePositionAndSize"));
    require(override.type == SettingType::Toggle, QStringLiteral("geometry override should be a toggle"));
    require(override.normalizer == SettingNormalizer::Bool,
        QStringLiteral("geometry override should use boolean normalization"));
    require(!settingDefaultValue(override).toBool(), QStringLiteral("geometry override should default to false"));
    require(override.persisted, QStringLiteral("geometry override should be persisted"));
    require(override.platform == SettingPlatform::All,
        QStringLiteral("geometry override should be available on every platform"));
    require(QLatin1String(override.group) == QLatin1String("Subtitle Appearance"),
        QStringLiteral("geometry override should belong to Subtitle Appearance"));
    require(override.level == SettingLevel::Essential,
        QStringLiteral("fixed-position override should be available in basic subtitle settings"));
    const SettingSpec& colorOverride = requiredSpec(QStringLiteral("subtitles/overrideTextColor"));
    require(colorOverride.type == SettingType::Toggle && colorOverride.level == SettingLevel::Essential,
        QStringLiteral("text colour override should be available in basic subtitle settings"));
}

void schemaModelExposesEverySpecOnce()
{
    const QVariantList model = settingSchemaModel();
    require(
        model.size() == settingSpecs().size(), QStringLiteral("schema model did not expose one row per setting spec"));

    QSet<QString> modelKeys;
    for (const QVariant& item : model) {
        const QVariantMap row = item.toMap();
        const QString key = row.value(QStringLiteral("key")).toString();
        require(!key.isEmpty(), QStringLiteral("schema row had no key"));
        require(!modelKeys.contains(key), QStringLiteral("schema model exposed duplicate row for %1").arg(key));
        modelKeys.insert(key);

        const SettingSpec& spec = requiredSpec(key);
        require(row.value(QStringLiteral("source")).toString()
                == (spec.persisted ? QStringLiteral("settings") : QStringLiteral("page")),
            QStringLiteral("schema row source diverged for %1").arg(key));
    }

    require(
        modelKeys.size() == settingSpecs().size(), QStringLiteral("schema model key set did not match setting specs"));
    for (const SettingSpec& spec : settingSpecs()) {
        const QString key = keyString(spec);
        require(modelKeys.contains(key), QStringLiteral("schema model missed setting row %1").arg(key));
    }

    for (const QString& obsoleteKey : { QStringLiteral("subtitles/burnIn"), QStringLiteral("subtitles/renderPgs"),
             QStringLiteral("subtitles/alwaysBurnInWhenTranscoding") }) {
        require(findSettingSpec(obsoleteKey) == nullptr,
            QStringLiteral("obsolete server-policy setting remained in the schema: %1").arg(obsoleteKey));
    }
}

// The page renders rows in declaration order, so a group's rows have to stay
// together or its header would repeat further down the list.
void groupsAreDeclaredContiguously()
{
    QSet<QString> closedGroups;
    QString currentGroup;
    for (const SettingSpec& spec : settingSpecs()) {
        const QString group = QString::fromLatin1(spec.group);
        if (group == currentGroup)
            continue;
        require(!closedGroups.contains(group), QStringLiteral("settings group %1 was declared in two runs").arg(group));
        if (!currentGroup.isEmpty())
            closedGroups.insert(currentGroup);
        currentGroup = group;
    }
}
void pageRowsShareTheSchemaContract()
{
    const QStringList pageKeys { QStringLiteral("session/account"), QStringLiteral("action/switchUser"),
        QStringLiteral("action/logout"), QStringLiteral("i18n/locale"), QStringLiteral("theme/accent"),
        QStringLiteral("theme/reducedMotion"), QStringLiteral("theme/railLabels"), QStringLiteral("theme/renderMode"),
        QStringLiteral("theme/antialiasedText"), QStringLiteral("theme/technicalMetadata"),
        QStringLiteral("action/subtitleSettings"), QStringLiteral("action/resetSubtitleAppearance"),
        QStringLiteral("about/version"), QStringLiteral("action/openSourceNotices"), QStringLiteral("about/locale"),
        QStringLiteral("shell/diagnostics"), QStringLiteral("shell/latencyGuard"),
        QStringLiteral("shell/latencyOverlay"), QStringLiteral("action/clearLatencyStatistics") };
    for (const QString& key : pageKeys) {
        const SettingSpec& spec = requiredSpec(key);
        require(!spec.persisted, QStringLiteral("page-owned row %1 must not be persisted").arg(key));
        require(schemaRow(key).value(QStringLiteral("source")).toString() == QStringLiteral("page"),
            QStringLiteral("page-owned row %1 was missing from the schema model").arg(key));
    }
    const QHash<QString, QString> accentChoices = choicesByLabelFromRow(schemaRow(QStringLiteral("theme/accent")));
    require(accentChoices.size() == 3 && accentChoices.value(QStringLiteral("0")) == QStringLiteral("Blue")
            && accentChoices.value(QStringLiteral("1")) == QStringLiteral("Purple")
            && accentChoices.value(QStringLiteral("2")) == QStringLiteral("Indigo"),
        QStringLiteral("accent colour choices were missing instead of falling back to the default palette"));
    require(requiredSpec(QStringLiteral("theme/railLabels")).level == SettingLevel::Advanced,
        QStringLiteral("rail label tuning should be hidden at Essential detail"));
    require(requiredSpec(QStringLiteral("shell/diagnostics")).level == SettingLevel::Expert,
        QStringLiteral("diagnostics controls should be hidden below Expert detail"));
}

void subtitleChoicesExplainTheirBehavior()
{
    const SettingSpec& subtitleMode = requiredSpec(QStringLiteral("subtitles/mode"));
    const QString smartLabel = choiceLabel(subtitleMode, QStringLiteral("Smart"));
    require(smartLabel.contains(QStringLiteral("another language")),
        QStringLiteral("Smart subtitle mode should explain the audio-language condition"));
    require(smartLabel.contains(QStringLiteral("%1")),
        QStringLiteral("Smart subtitle mode should carry the preferred-language placeholder"));

    const SettingSpec& audioTrackMode = requiredSpec(QStringLiteral("audio/trackMode"));
    require(choiceLabel(audioTrackMode, QStringLiteral("Smart")).contains(QStringLiteral("%1")),
        QStringLiteral("Smart audio mode should carry the preferred-language placeholder"));

    for (const QString& value : choiceValues(subtitleMode)) {
        require(!choiceLabel(subtitleMode, value).isEmpty(),
            QStringLiteral("subtitle mode %1 should have a label").arg(value));
    }
    const QVariantMap hdrBrightness = schemaRow(QStringLiteral("subtitles/hdrBrightnessPercent"));
    require(hdrBrightness.value(QStringLiteral("title")).toString() == QStringLiteral("HDR Subtitle Brightness"),
        QStringLiteral("HDR brightness should use the subtitle-facing label"));
    require(hdrBrightness.value(QStringLiteral("requiresHdrPlayback")).toBool()
            && hdrBrightness.value(QStringLiteral("dependsOnKey")).toString().isEmpty(),
        QStringLiteral("HDR brightness should be available without a separate enable toggle"));
    require(hdrBrightness.value(QStringLiteral("defaultValue")).toInt() == 50
            && hdrBrightness.value(QStringLiteral("from")).toInt() == 5,
        QStringLiteral("HDR brightness should default to 50% and allow 5%"));
    const QVariantMap hdrPeak = schemaRow(QStringLiteral("playback/hdrPeakNits"));
    require(hdrPeak.value(QStringLiteral("dependsOnKey")).toString().isEmpty(),
        QStringLiteral("display peak brightness should be reachable whenever HDR output can be forced on, "
                       "not only when it is left on automatic"));
    require(hdrPeak.value(QStringLiteral("defaultValue")).toInt() == 0,
        QStringLiteral("display peak brightness should default to asking the display"));
    const QVariantMap verticalPosition = schemaRow(QStringLiteral("subtitles/verticalPositionPercent"));
    require(verticalPosition.value(QStringLiteral("defaultValue")).toInt() == 95,
        QStringLiteral("vertical subtitle position should default to 95%"));
    const QVariantMap textSize = schemaRow(QStringLiteral("subtitles/scalePercent"));
    require(textSize.value(QStringLiteral("title")).toString() == QStringLiteral("Text Size"),
        QStringLiteral("subtitle scale should use the text-size label"));
    require(findSettingSpec(QStringLiteral("subtitles/textSize")) == nullptr,
        QStringLiteral("separate subtitle text-size choice should be removed"));
    require(findSettingSpec(QStringLiteral("subtitles/bitmapSmoothing")) == nullptr,
        QStringLiteral("discrete bitmap smoothing choice should be removed"));
    const QVariantMap bitmapSharpness = schemaRow(QStringLiteral("subtitles/bitmapSharpnessPercent"));
    require(bitmapSharpness.value(QStringLiteral("type")).toString() == QStringLiteral("slider")
            && bitmapSharpness.value(QStringLiteral("defaultValue")).toInt() == 45
            && bitmapSharpness.value(QStringLiteral("from")).toInt() == 0
            && bitmapSharpness.value(QStringLiteral("to")).toInt() == 100,
        QStringLiteral("bitmap sharpness should be a slightly-softened percentage slider"));
    const QVariantMap recolorImages = schemaRow(QStringLiteral("subtitles/recolorImageSubtitles"));
    require(recolorImages.value(QStringLiteral("type")).toString() == QStringLiteral("toggle")
            && recolorImages.value(QStringLiteral("choiceValues")).toList().isEmpty(),
        QStringLiteral("image subtitle recolouring should be a simple toggle"));
    const QVariantMap bitmapShadow = schemaRow(QStringLiteral("subtitles/bitmapShadowEnabled"));
    require(bitmapShadow.value(QStringLiteral("type")).toString() == QStringLiteral("toggle")
            && bitmapShadow.value(QStringLiteral("defaultValue")).toBool()
            && bitmapShadow.value(QStringLiteral("level")).toInt() == static_cast<int>(SettingLevel::Advanced),
        QStringLiteral("image subtitle shadow should be enabled by default and advanced"));
    const QVariantMap bitmapShadowSpread = schemaRow(QStringLiteral("subtitles/bitmapShadowSpreadSize"));
    require(bitmapShadowSpread.value(QStringLiteral("defaultValue")).toInt() == 6
            && bitmapShadowSpread.value(QStringLiteral("from")).toInt() == 1
            && bitmapShadowSpread.value(QStringLiteral("to")).toInt() == 16
            && bitmapShadowSpread.value(QStringLiteral("dependsOnKey")).toString()
                == QStringLiteral("subtitles/bitmapShadowEnabled"),
        QStringLiteral("wide image shadow should default to a mild six-pixel advanced layer"));
    const QVariantMap blackBars = schemaRow(QStringLiteral("subtitles/allowInBlackBars"));
    require(blackBars.value(QStringLiteral("type")).toString() == QStringLiteral("toggle")
            && blackBars.value(QStringLiteral("defaultValue")).toBool(),
        QStringLiteral("black-bar subtitle placement should be an enabled-by-default toggle"));
}

void systemLanguageLabelNamesResolvedLanguage()
{
    LocalizationManager localization;
    const QString expectedLanguage = QLocale::languageToString(QLocale::system().language());
    require(
        localization.displayNameFor(QStringLiteral("system")) == QStringLiteral("System (%1)").arg(expectedLanguage),
        QStringLiteral("system language choice should name the resolved language"));
}

void buttonChoicesAndLabelsExposePlayerActions()
{
    const QStringList buttonKeys {
        QStringLiteral("input/redButton"),
        QStringLiteral("input/greenButton"),
        QStringLiteral("input/yellowButton"),
        QStringLiteral("input/blueButton"),
    };
    const QStringList expectedActions {
        QStringLiteral("none"),
        QStringLiteral("togglePause"),
        QStringLiteral("toggleSubs"),
        QStringLiteral("cycleSubs"),
        QStringLiteral("cycleAudio"),
        QStringLiteral("queuePrevious"),
        QStringLiteral("queueNext"),
        QStringLiteral("skipBack10"),
        QStringLiteral("skipForward10"),
        QStringLiteral("skipBack30"),
        QStringLiteral("skipForward30"),
        QStringLiteral("skipBack90"),
        QStringLiteral("skipForward90"),
        QStringLiteral("skipBackAndEnableSubs"),
        QStringLiteral("skipSegment"),
        QStringLiteral("showInfo"),
        QStringLiteral("stop"),
    };
    const QSet<QString> expectedActionSet = stringSet(expectedActions);

    for (const QString& key : buttonKeys) {
        const SettingSpec& spec = requiredSpec(key);
        require(spec.choiceCount == expectedActions.size(),
            QStringLiteral("button setting %1 exposed an unexpected action count").arg(key));
        require(stringSet(choiceValues(spec)) == expectedActionSet,
            QStringLiteral("button setting %1 did not expose the expected actions").arg(key));

        const QVariantMap row = schemaRow(key);
        require(!row.isEmpty(), QStringLiteral("schema model missed button row %1").arg(key));
        const QHash<QString, QString> modelLabels = choicesByLabelFromRow(row);
        require(modelLabels.size() == expectedActions.size(),
            QStringLiteral("schema model button labels changed for %1").arg(key));

        require(choiceLabel(spec, QStringLiteral("togglePause")) == QStringLiteral("Play / Pause"),
            QStringLiteral("togglePause label changed"));
        require(choiceLabel(spec, QStringLiteral("toggleSubs")) == QStringLiteral("Toggle subtitles"),
            QStringLiteral("toggleSubs label changed"));
        require(choiceLabel(spec, QStringLiteral("cycleAudio")) == QStringLiteral("Cycle audio track"),
            QStringLiteral("cycleAudio label changed"));
        require(choiceLabel(spec, QStringLiteral("skipBackAndEnableSubs"))
                == QStringLiteral("Skip back 10 s + enable subs"),
            QStringLiteral("skipBackAndEnableSubs label changed"));
        require(choiceLabel(spec, QStringLiteral("skipSegment")) == QStringLiteral("Skip intro / outro"),
            QStringLiteral("skipSegment label changed"));
        require(choiceLabel(spec, QStringLiteral("stop")) == QStringLiteral("Stop playback"),
            QStringLiteral("stop label changed"));

        require(modelLabels.value(QStringLiteral("togglePause")) == QStringLiteral("Play / Pause"),
            QStringLiteral("schema model togglePause label changed"));
        require(modelLabels.value(QStringLiteral("skipBackAndEnableSubs"))
                == QStringLiteral("Skip back 10 s + enable subs"),
            QStringLiteral("schema model skipBackAndEnableSubs label changed"));
        require(modelLabels.value(QStringLiteral("skipSegment")) == QStringLiteral("Skip intro / outro"),
            QStringLiteral("schema model skipSegment label changed"));
    }
}

} // namespace

JELLYFIN_TEST_MAIN("settings-schema")
{
    QCoreApplication app(argc, argv);
    requiredPersistedKeysArePresentExactlyOnce();
    audioOutputChoicesMatchPlatform();
    resolutionAndBitrateAreSettledSeparately();
    normalizersPreservePersistedValueSemantics();
    subtitleGeometryOverrideMatchesSchemaContract();
    schemaModelExposesEverySpecOnce();
    groupsAreDeclaredContiguously();
    pageRowsShareTheSchemaContract();
    subtitleChoicesExplainTheirBehavior();
    systemLanguageLabelNamesResolvedLanguage();
    buttonChoicesAndLabelsExposePlayerActions();
    return EXIT_SUCCESS;
}
