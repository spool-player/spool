#include "SettingsController.h"

#include "../cache/DatabaseManager.h"
#include "../common/AsyncTask.h"
#include "../diagnostics/InputLatencyMonitor.h"
#include "../platform/PlatformSettingsPolicy.h"
#include "../player/PlayerController.h"
#include "../player/RenderTargetProfile.h"
#include "../provider/ProviderRegistry.h"
#include "ArtworkService.h"
#include "LocalizationManager.h"
#include "SettingsSchema.h"
#include "SettingsSyncController.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <stdexcept>

#include <QDebug>
#include <QLocale>
#include <QSet>

#include <iterator>

namespace Spool {

namespace {

    constexpr auto kPlayerControlTooltipSessionsKey = "player/controlTooltipSessions";
    constexpr int kPlayerControlTooltipSessionLimit = 3;

    QString keyString(const SettingSpec& spec)
    {
        return QString::fromLatin1(spec.key);
    }

    const SettingSpec& specForKey(const char *key)
    {
        const SettingSpec *spec = findSettingSpec(QString::fromLatin1(key));
        Q_ASSERT(spec);
        return *spec;
    }

    bool externalStore(const SettingSpec& spec)
    {
        return spec.target == SettingTarget::Locale || spec.target == SettingTarget::LatencyGuard
            || spec.target == SettingTarget::LatencyOverlay;
    }

    bool trackDefault(const SettingSpec& spec)
    {
        return spec.nativePreference[0] != '\0';
    }

    QString applicationKey(const QString& key)
    {
        return QStringLiteral("settings/application/") + key;
    }

    QString encodeApplication(const QVariantMap& record)
    {
        return QString::fromUtf8(QJsonDocument::fromVariant(record).toJson(QJsonDocument::Compact));
    }

} // namespace

SettingsController::SettingsController(
    DatabaseManager *database, PlayerController *player, ArtworkService *artwork, QObject *parent)
    : QObject(parent)
    , m_database(database)
    , m_player(player)
    , m_artwork(artwork)
    , m_uiScalePercent(platformDefaultUiScalePercent())
{
    if (m_player) {
        connect(m_player, &PlayerController::sessionActiveChanged, this, [this] {
            if (!m_player->sessionActive())
                Async::runScoped(
                    this, applyDeferredTrackDefaults(), [] {},
                    [this](const std::exception_ptr& error) { emit errorOccurred(exceptionMessage(error)); },
                    "apply deferred track defaults");
        });
    }
}

QStringList SettingsController::subtitleLanguageOptions() const
{
    loadSubtitleLanguages();
    return m_subtitleLanguageLabels;
}
int SettingsController::subtitleLanguageIndex() const
{
    loadSubtitleLanguages();
    const int index = m_subtitleLanguageCodes.indexOf(m_subtitlePreferences.language);
    return index >= 0 ? index : 0;
}

int SettingsController::audioLanguageIndex() const
{
    loadSubtitleLanguages();
    return qMax(0, m_subtitleLanguageCodes.indexOf(m_subtitlePreferences.audioLanguage));
}

void SettingsController::setAudioLanguageIndex(int index)
{
    loadSubtitleLanguages();
    if (index >= 0 && index < m_subtitleLanguageCodes.size())
        setValue(QStringLiteral("audio/language"), m_subtitleLanguageCodes.at(index));
}

void SettingsController::attachLocalization(LocalizationManager *localization)
{
    if (m_localization)
        disconnect(m_localization, nullptr, this, nullptr);
    m_localization = localization;
    if (!localization)
        return;
    const auto mirror = [this] {
        mirrorExternalValue(QStringLiteral("i18n/locale"),
            m_localization->useSystemLocale() ? QStringLiteral("system") : m_localization->currentLocale());
    };
    connect(localization, &LocalizationManager::localeChanged, this, mirror);
    mirror();
}

void SettingsController::attachInputLatency(InputLatencyMonitor *monitor)
{
    if (m_inputLatency)
        disconnect(m_inputLatency, nullptr, this, nullptr);
    m_inputLatency = monitor;
    if (!monitor)
        return;
    connect(monitor, &InputLatencyMonitor::enabledChanged, this,
        [this] { mirrorExternalValue(QStringLiteral("shell/latencyGuard"), m_inputLatency->enabled()); });
    connect(monitor, &InputLatencyMonitor::overlayEnabledChanged, this,
        [this] { mirrorExternalValue(QStringLiteral("shell/latencyOverlay"), m_inputLatency->overlayEnabled()); });
    mirrorExternalValue(QStringLiteral("shell/latencyGuard"), monitor->enabled());
    mirrorExternalValue(QStringLiteral("shell/latencyOverlay"), monitor->overlayEnabled());
}

void SettingsController::attachSync(SettingsSyncController *sync)
{
    m_sync = sync;
}

void SettingsController::mirrorExternalValue(const QString& key, const QVariant& value)
{
    if (m_applyingExternal || m_values.value(key) == value)
        return;
    // Internal monitor/locale changes are not committed user edits.
    m_commitGenerations[key] = ++m_commitGeneration;
    m_values.insert(key, value);
    emit settingChanged(key);
    emit settingsValuesChanged();
}

void SettingsController::applyExternalValue(const SettingSpec& spec, const QVariant& value)
{
    m_applyingExternal = true;
    if (spec.target == SettingTarget::Locale && m_localization) {
        if (value.toString().isEmpty())
            m_localization->useSystemDefault();
        else
            m_localization->setLocale(value.toString());
    } else if (spec.target == SettingTarget::LatencyGuard && m_inputLatency) {
        m_inputLatency->setEnabled(value.toBool());
    } else if (spec.target == SettingTarget::LatencyOverlay && m_inputLatency) {
        m_inputLatency->setOverlayEnabled(value.toBool());
    }
    m_applyingExternal = false;
    QSettings settings;
    settings.sync();
    if (settings.status() != QSettings::NoError)
        throw std::runtime_error("Could not persist external settings");
}

bool SettingsController::supportsSyncValue(const QString& key, const QVariant& value) const
{
    const SettingSpec *spec = findSettingSpec(key);
    if (!spec || !settingAcceptsRemoteValue(*spec, value))
        return false;
    if (spec->target == SettingTarget::Locale)
        return m_localization
            && (value.toString().isEmpty() || m_localization->availableLocales().contains(value.toString()));
    if (spec->target == SettingTarget::SubtitleFont && value.toString().startsWith(QStringLiteral("system:")))
        return systemSubtitleFonts().contains(value.toString().mid(7));
    return true;
}

QStringList SettingsController::systemSubtitleFonts() const
{
    // Both CONSTANT list properties below must be cached: QML re-invokes the
    // READ accessor for every element access on value-type sequences, so an
    // uncached build turns a JS iteration into a full rebuild per element.
    static const QStringList families = platformSystemSubtitleFonts();
    return families;
}

QVariantList SettingsController::settingsSchema() const
{
    // Cached for the same reason as the fonts above, and because the row set
    // is fixed once the provider is chosen.
    if (m_schema.isEmpty())
        m_schema = settingSchemaModel();
    return m_schema;
}

QVariantMap SettingsController::values() const
{
    return m_values;
}

QVariant SettingsController::value(const QString& key) const
{
    if (m_values.contains(key))
        return m_values.value(key);
    if (const SettingSpec *spec = findSettingSpec(key))
        return settingDefaultValue(*spec);
    return {};
}

QString SettingsController::audioDelayTargetLabel() const
{
    return platformAudioRouteDisplayName(m_currentAudioOutput);
}

QStringList SettingsController::localSettingKeys()
{
    QStringList keys;
    keys.reserve(static_cast<qsizetype>(settingSpecs().size()) + 1);
    for (const SettingSpec& spec : settingSpecs()) {
        if (!spec.persisted)
            continue;
        if (externalStore(spec) || trackDefault(spec))
            keys.append(applicationKey(keyString(spec)));
        if (!externalStore(spec) && !(platformUsesPerOutputAudioDelay() && spec.target == SettingTarget::AudioDelay))
            keys.append(keyString(spec));
    }
    keys.append(QString::fromLatin1(kPlayerControlTooltipSessionsKey));
    return keys;
}

QCoro::Task<void> SettingsController::loadLocalAsync()
{
    applyLocalValues(co_await m_database->loadValuesAsync(localSettingKeys()));
}

QString SettingsController::stepDownRenderQuality()
{
    if (!m_autoAdjustRenderQuality)
        return {};
    // Ordered worst-effort-last; there is nowhere to go from the bottom.
    static constexpr const char *rungs[] = { "maximum", "high", "balanced", "fast" };
    constexpr int rungCount = static_cast<int>(std::size(rungs));
    int current = rungCount - 1;
    for (int index = 0; index < rungCount; ++index) {
        if (m_renderQuality == QLatin1String(rungs[index])) {
            current = index;
            break;
        }
    }
    if (current >= rungCount - 1) {
        // Nothing cheaper to render with. On a platform that can hand the
        // decoder its own surface, the next step is to stop rendering
        // ourselves at all.
        if (m_videoOutputMode == QLatin1String("direct") || !platformSupportsDirectVideoOutput())
            return {};
        submitValues({ { QStringLiteral("playback/videoOutput"), QStringLiteral("direct") } }, ChangeOrigin::Automatic);
        return QStringLiteral("direct");
    }
    const QString next = QString::fromLatin1(rungs[current + 1]);
    submitValues({ { QStringLiteral("playback/renderQuality"), next } }, ChangeOrigin::Automatic);
    return next;
}

void SettingsController::applyLocalValues(const QVariantMap& storedValues)
{

    for (const SettingSpec& spec : settingSpecs()) {
        if (!spec.persisted)
            continue;
        const QString key = keyString(spec);
        if (externalStore(spec) || trackDefault(spec)) {
            const auto record = QJsonDocument::fromJson(storedValues.value(applicationKey(key)).toString().toUtf8())
                                    .toVariant()
                                    .toMap();
            if (!record.isEmpty()) {
                m_pendingApplications.insert(key, record);
                const quint64 generation = record.value(QStringLiteral("generation")).toString().toULongLong();
                m_commitGeneration = qMax(m_commitGeneration, generation);
                m_commitGenerations.insert(key, generation);
            }
        }
        if (externalStore(spec))
            continue;
        if (platformUsesPerOutputAudioDelay() && spec.target == SettingTarget::AudioDelay) {
            const QVariant normalized = normalizedSettingValue(spec, settingDefaultValue(spec));
            m_values.insert(key, normalized);
            applySchemaValue(spec, normalized, false);
            continue;
        }
        const QVariant rawValue = storedValues.value(key);
        QVariant defaultValue = settingDefaultValue(spec);
        if (spec.target == SettingTarget::RemoteControlTargetEnabled)
            defaultValue = platformDefaultRemoteControlTargetEnabled();
        else if (spec.target == SettingTarget::RenderQuality)
            defaultValue = QString::fromLatin1(platformDefaultRenderQuality());
        else if (spec.target == SettingTarget::VideoOutputMode)
            defaultValue = QString::fromLatin1(platformDefaultVideoOutput());
        const QVariant stored = !rawValue.isValid() || rawValue.toString().isEmpty() ? defaultValue : rawValue;
        const QVariant normalized = normalizedSettingValue(spec, stored);
        m_values.insert(key, normalized);
        applySchemaValue(spec, normalized, false);

        const QString serialized = serializedSettingValue(spec, normalized);
        if (serialized != stored.toString())
            m_database->saveSetting(key, serialized);
    }
    bool tooltipCountValid = false;
    const int tooltipSessions
        = storedValues.value(QString::fromLatin1(kPlayerControlTooltipSessionsKey)).toInt(&tooltipCountValid);
    m_playerControlTooltipSessions
        = tooltipCountValid ? qBound(0, tooltipSessions, kPlayerControlTooltipSessionLimit) : 0;

    MpvConfigPolicy configPolicy = validatedPlatformMpvConfigPolicy(m_mpvConfigMode, m_mpvConfigDirectory);
    if (!configPolicy.valid) {
        qWarning() << "settings: invalid persisted mpv configuration policy; disabling custom config";
        m_mpvConfigMode = QStringLiteral("disabled");
        m_mpvConfigDirectory.clear();
        m_values.insert(QStringLiteral("playback/mpvConfigMode"), m_mpvConfigMode);
        m_values.insert(QStringLiteral("playback/mpvConfigDirectory"), m_mpvConfigDirectory);
        m_database->saveSetting(QStringLiteral("playback/mpvConfigMode"), m_mpvConfigMode);
        m_database->saveSetting(QStringLiteral("playback/mpvConfigDirectory"), m_mpvConfigDirectory);
        configPolicy = validatedPlatformMpvConfigPolicy(m_mpvConfigMode, m_mpvConfigDirectory);
    } else if (configPolicy.mode == MpvConfigPolicy::Mode::Custom && configPolicy.directory != m_mpvConfigDirectory) {
        m_mpvConfigDirectory = configPolicy.directory;
        m_values.insert(QStringLiteral("playback/mpvConfigDirectory"), m_mpvConfigDirectory);
        m_database->saveSetting(QStringLiteral("playback/mpvConfigDirectory"), m_mpvConfigDirectory);
    }

    m_localSettingsLoaded = true;
    if (platformUsesPerOutputAudioDelay())
        loadCurrentAudioDelay();

    if (m_player) {
        m_player->setNightModeEnabled(m_nightModeEnabled);
        m_player->setToneMappingVisualizationEnabled(m_toneMappingVisualizationEnabled);
        applyAudioDelayToPlayer();
        m_player->setAudioOutputMode(m_audioOutputMode);
        m_player->setForwardCacheSizeMiB(m_forwardCacheSizeMiB);
        m_player->setMpvConfigPolicy(configPolicy);
    }
    applyArtworkEncoding();
    applyPlaybackPreferences();
    if (m_player)
        m_player->setSubtitlePreferences(m_subtitlePreferences);

    emit settingsValuesChanged();
    emit nightModeChanged();
    emit audioDelayChanged();
    emit subtitleSettingsChanged();
    emit buttonRemapChanged();
    emit appearanceChanged();
    emit remoteControlSettingsChanged();
}

void SettingsController::loadSubtitleLanguages() const
{
    if (m_subtitleLanguageCodes.size() > 1)
        return;
    // Every language Qt knows, by display name; the preference is stored as
    // the ISO 639-2 code players match against.
    QList<std::pair<QString, QString>> languages;
    for (int value = QLocale::Abkhazian; value <= QLocale::LastLanguage; ++value) {
        const auto language = static_cast<QLocale::Language>(value);
        const QString code = QLocale::languageToCode(language, QLocale::ISO639Part2);
        if (code.size() == 3)
            languages.append({ QLocale::languageToString(language), code });
    }
    std::sort(languages.begin(), languages.end(),
        [](const auto& a, const auto& b) { return a.first.localeAwareCompare(b.first) < 0; });
    for (const auto& [label, code] : std::as_const(languages)) {
        m_subtitleLanguageCodes.push_back(code);
        m_subtitleLanguageLabels.push_back(label);
    }
}

void SettingsController::completePlayerControlTooltipSession()
{
    if (m_playerControlTooltipSessions >= kPlayerControlTooltipSessionLimit)
        return;
    ++m_playerControlTooltipSessions;
    m_database->saveSetting(
        QString::fromLatin1(kPlayerControlTooltipSessionsKey), QString::number(m_playerControlTooltipSessions));
    if (m_playerControlTooltipSessions == kPlayerControlTooltipSessionLimit)
        emit playerControlTooltipsEnabledChanged();
}

void SettingsController::setValue(const QString& key, const QVariant& value)
{
    submitValues({ { key, value } }, ChangeOrigin::User);
}

void SettingsController::submitValues(QVariantMap values, ChangeOrigin origin)
{
    Async::runScoped(
        this, applyValues(std::move(values), origin), [] {},
        [this](const std::exception_ptr& error) { emit errorOccurred(exceptionMessage(error)); }, "save settings");
}

QCoro::Task<void> SettingsController::applyValues(
    QVariantMap values, ChangeOrigin origin, QVariantMap additionalSerializedSettings)
{
    if (origin == ChangeOrigin::Preview) {
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            previewValue(it.key(), it.value());
        co_return;
    }
    QVariantMap normalized;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const SettingSpec *spec = findSettingSpec(it.key());
        if (!spec || !spec->persisted)
            continue;
        if (origin == ChangeOrigin::RemoteSync && !supportsSyncValue(it.key(), it.value()))
            continue;
        if (externalStore(*spec)
            && ((spec->target == SettingTarget::Locale && !m_localization)
                || (spec->target != SettingTarget::Locale && !m_inputLatency)))
            continue;
        QVariant result = normalizedSettingValue(*spec, it.value());
        if (spec->target == SettingTarget::MpvConfigDirectory) {
            const auto policy = validatedPlatformMpvConfigPolicy(QStringLiteral("custom"), result.toString());
            if (!policy.valid)
                throw std::runtime_error(policy.error.toStdString());
            result = policy.directory;
        }
        normalized.insert(it.key(), result);
    }
    if (normalized.contains(QStringLiteral("playback/mpvConfigMode"))) {
        const auto policy
            = validatedPlatformMpvConfigPolicy(normalized.value(QStringLiteral("playback/mpvConfigMode")).toString(),
                normalized.value(QStringLiteral("playback/mpvConfigDirectory"), m_mpvConfigDirectory).toString());
        if (!policy.valid)
            throw std::runtime_error(policy.error.toStdString());
    }

    const quint64 generation = ++m_commitGeneration;
    QVariantMap changed;
    QVariantMap external;
    QVariantMap serialized = std::move(additionalSerializedSettings);
    for (auto it = normalized.cbegin(); it != normalized.cend(); ++it) {
        const auto& spec = *findSettingSpec(it.key());
        const bool hadPending = m_pendingApplications.contains(it.key());
        if (hadPending && (!externalStore(spec) || origin == ChangeOrigin::User)) {
            m_pendingApplications.remove(it.key());
            serialized.insert(applicationKey(it.key()), QString());
        }
        if (value(it.key()) == it.value() && !hadPending)
            continue;
        m_commitGenerations.insert(it.key(), generation);
        changed.insert(it.key(), it.value());
        const bool deferred
            = origin == ChangeOrigin::RemoteSync && trackDefault(spec) && m_player && m_player->sessionActive();
        if (externalStore(spec) || deferred) {
            const QVariantMap record { { QStringLiteral("value"), it.value() },
                { QStringLiteral("origin"), static_cast<int>(origin) },
                { QStringLiteral("generation"), QString::number(generation) },
                { QStringLiteral("accountId"), m_sync ? m_sync->accountId() : QString() },
                { QStringLiteral("deferred"), deferred } };
            m_pendingApplications.insert(it.key(), record);
            serialized.insert(applicationKey(it.key()), encodeApplication(record));
            if (!deferred)
                external.insert(it.key(), it.value());
        } else {
            const QString storageKey = spec.target == SettingTarget::AudioDelay && platformUsesPerOutputAudioDelay()
                ? platformAudioDelayStorageKey(m_currentAudioOutput)
                : it.key();
            serialized.insert(storageKey, serializedSettingValue(spec, it.value()));
            if (spec.target == SettingTarget::AudioDelay)
                m_audioOutputLoadGeneration.invalidate();
            m_values.insert(it.key(), it.value());
        }
    }
    // Mutate all members before applying effects, so each effect observes the
    // complete normalized batch rather than half of a preference pair.
    m_batchEffects = true;
    QVariantMap immediate;
    for (auto it = changed.cbegin(); it != changed.cend(); ++it) {
        if (external.contains(it.key()) || m_pendingApplications.contains(it.key()))
            continue;
        immediate.insert(it.key(), it.value());
        applySchemaValue(*findSettingSpec(it.key()), it.value(), false);
    }
    for (auto it = immediate.cbegin(); it != immediate.cend(); ++it)
        applySchemaValue(*findSettingSpec(it.key()), it.value(), true);
    m_batchEffects = false;
    bool subtitleEffects = false;
    bool artworkEffects = false;
    bool streamingEffects = false;
    for (auto it = immediate.cbegin(); it != immediate.cend(); ++it) {
        const auto& spec = *findSettingSpec(it.key());
        subtitleEffects |= it.key().startsWith(QStringLiteral("subtitles/")) || trackDefault(spec);
        artworkEffects |= it.key().startsWith(QStringLiteral("artwork/"));
        streamingEffects |= spec.target == SettingTarget::MaxStreamingHeight
            || spec.target == SettingTarget::ManualStreamingBitrate || spec.target == SettingTarget::MaxStreamingBitrate
            || spec.target == SettingTarget::UnlimitedLocalBitrate || spec.target == SettingTarget::PreferRemux;
    }
    if (subtitleEffects)
        applySubtitlePreferencesToPlayer();
    if (artworkEffects)
        applyArtworkEncoding();
    if (streamingEffects)
        applyPlaybackPreferences();

    const bool userCommit = origin == ChangeOrigin::User && !changed.isEmpty();
    if (userCommit && m_sync) {
        const QVariantMap ledger = m_sync->prepareLocalCommit(changed);
        for (auto it = ledger.cbegin(); it != ledger.cend(); ++it)
            serialized.insert(it.key(), it.value());
    }
    if (!immediate.isEmpty()) {
        for (auto it = immediate.cbegin(); it != immediate.cend(); ++it)
            emit settingChanged(it.key());
        emitBatchSignals(immediate);
        // Mixed external/SQLite batches notify after external application.
        if (external.isEmpty())
            emit settingsValuesChanged();
    }
    QPointer<SettingsController> guard(this);
    for (auto it = serialized.cbegin(); it != serialized.cend(); ++it)
        m_serializedGenerations.insert(it.key(), generation);
    try {
        if (!serialized.isEmpty())
            co_await m_database->saveSettings(serialized);
        if (!guard)
            co_return;
        for (auto it = serialized.cbegin(); it != serialized.cend(); ++it) {
            if (m_serializedGenerations.value(it.key()) == generation)
                m_failedSerializedSettings.remove(it.key());
        }
        QVariantMap cleared;
        bool externalChanged = false;
        for (auto it = external.cbegin(); it != external.cend(); ++it) {
            if (m_commitGenerations.value(it.key()) != generation)
                continue;
            applyExternalValue(*findSettingSpec(it.key()), it.value());
            m_values.insert(it.key(), it.value());
            m_pendingApplications.remove(it.key());
            cleared.insert(applicationKey(it.key()), QString());
            externalChanged = true;
            emit settingChanged(it.key());
        }
        if (externalChanged || (!external.isEmpty() && !immediate.isEmpty()))
            emit settingsValuesChanged();
        if (!cleared.isEmpty()) {
            serialized = cleared;
            co_await m_database->saveSettings(std::move(cleared));
        }
        if (guard && userCommit)
            emit userValuesCommitted(changed);
    } catch (...) {
        if (guard) {
            for (auto it = serialized.cbegin(); it != serialized.cend(); ++it) {
                if (m_serializedGenerations.value(it.key()) != generation)
                    continue;
                m_failedSerializedSettings.insert(it.key(), it.value());
                if (it.key().startsWith(QStringLiteral("settings/application/")) && !it.value().toString().isEmpty()) {
                    const QString key = it.key().mid(21);
                    if (m_commitGenerations.value(key) == generation)
                        m_pendingApplications.insert(
                            key, QJsonDocument::fromJson(it.value().toString().toUtf8()).toVariant().toMap());
                }
            }
        }
        if (guard && userCommit) {
            emit userValuesCommitFailed();
            emit settingsPersistenceFailed(exceptionMessage(std::current_exception()));
        }
        throw;
    }
}

void SettingsController::cancelRemoteApplications(const QString& key, bool discardSyncLedger)
{
    // Control changes persist a new ledger through the sync controller. A
    // retry of an older failed settings transaction must not resurrect it.
    for (auto it = m_serializedGenerations.begin(); it != m_serializedGenerations.end(); ++it) {
        if (discardSyncLedger && it.key().startsWith(QStringLiteral("settingsSync/"))) {
            it.value() = ++m_commitGeneration;
            m_failedSerializedSettings.remove(it.key());
        }
    }
    QVariantMap cleared;
    for (auto it = m_pendingApplications.begin(); it != m_pendingApplications.end();) {
        const QVariantMap record = it.value().toMap();
        if ((key.isEmpty() || it.key() == key)
            && record.value(QStringLiteral("origin")).toInt() == static_cast<int>(ChangeOrigin::RemoteSync)) {
            m_commitGenerations[it.key()] = ++m_commitGeneration;
            cleared.insert(applicationKey(it.key()), QString());
            m_serializedGenerations[applicationKey(it.key())] = m_commitGeneration;
            m_failedSerializedSettings.remove(applicationKey(it.key()));
            it = m_pendingApplications.erase(it);
        } else {
            ++it;
        }
    }
    if (!cleared.isEmpty())
        Async::runScoped(
            this, applyValues({}, ChangeOrigin::Initialization, std::move(cleared)), [] {},
            [this](const std::exception_ptr& error) { emit settingsPersistenceFailed(exceptionMessage(error)); },
            "invalidate pending remote application");
}

QCoro::Task<void> SettingsController::retryPendingPersistence()
{
    QPointer<SettingsController> guard(this);
    const QVariantMap retry = m_failedSerializedSettings;
    if (!retry.isEmpty())
        co_await m_database->saveSettings(retry);
    if (!guard)
        co_return;
    for (auto it = retry.cbegin(); it != retry.cend(); ++it) {
        if (m_failedSerializedSettings.value(it.key()) == it.value())
            m_failedSerializedSettings.remove(it.key());
    }
    co_await recoverUnfinishedApplications();
}

QCoro::Task<void> SettingsController::recoverUnfinishedApplications()
{
    QPointer<SettingsController> guard(this);
    const QVariantMap pending = m_pendingApplications;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        if (!guard)
            co_return;
        const SettingSpec *spec = findSettingSpec(it.key());
        const QVariantMap record = it.value().toMap();
        const quint64 generation = record.value(QStringLiteral("generation")).toString().toULongLong();
        if (!spec || m_commitGenerations.value(it.key()) != generation)
            continue;
        const bool remote
            = record.value(QStringLiteral("origin")).toInt() == static_cast<int>(ChangeOrigin::RemoteSync);
        if (remote
            && (!m_sync
                || !m_sync->remoteApplicationAllowed(it.key(), record.value(QStringLiteral("accountId")).toString()))) {
            cancelRemoteApplications(it.key());
            continue;
        }
        if (remote && !m_sync->remoteApplicationReady())
            continue;
        if (record.value(QStringLiteral("deferred")).toBool() && m_player && m_player->sessionActive())
            continue;
        if (externalStore(*spec)) {
            if ((spec->target == SettingTarget::Locale && !m_localization)
                || (spec->target != SettingTarget::Locale && !m_inputLatency))
                continue;
            applyExternalValue(*spec, record.value(QStringLiteral("value")));
            m_values.insert(it.key(), record.value(QStringLiteral("value")));
            emit settingChanged(it.key());
            emit settingsValuesChanged();
        } else {
            m_pendingApplications.remove(it.key());
            co_await applyValues({ { it.key(), record.value(QStringLiteral("value")) } }, ChangeOrigin::Initialization,
                { { applicationKey(it.key()), QString() } });
            continue;
        }
        if (m_commitGenerations.value(it.key()) != generation && externalStore(*spec))
            continue;
        m_pendingApplications.remove(it.key());
        co_await applyValues({}, ChangeOrigin::Initialization, { { applicationKey(it.key()), QString() } });
    }
}

QCoro::Task<void> SettingsController::applyDeferredTrackDefaults()
{
    if (!m_sync || !m_sync->remoteApplicationReady())
        co_return;
    QVariantMap deferred;
    for (auto it = m_pendingApplications.cbegin(); it != m_pendingApplications.cend(); ++it) {
        const QVariantMap record = it.value().toMap();
        if (record.value(QStringLiteral("deferred")).toBool()
            && m_sync->remoteApplicationAllowed(it.key(), record.value(QStringLiteral("accountId")).toString()))
            deferred.insert(it.key(), record.value(QStringLiteral("value")));
    }
    if (deferred.isEmpty())
        co_return;
    // No suspension until applyValues has captured/invalidated every old
    // generation. Later local edits therefore win, including during its save.
    QVariantMap cleared;
    for (auto it = deferred.cbegin(); it != deferred.cend(); ++it) {
        m_pendingApplications.remove(it.key());
        cleared.insert(applicationKey(it.key()), QString());
    }
    co_await applyValues(std::move(deferred), ChangeOrigin::Initialization, std::move(cleared));
}

void SettingsController::previewValue(const QString& key, const QVariant& value)
{
    const SettingSpec *spec = findSettingSpec(key);
    if (!spec || !m_player)
        return;

    SubtitlePreferences preview = m_subtitlePreferences;
    const QVariant normalized = normalizedSettingValue(*spec, value);
    switch (spec->target) {
    case SettingTarget::SubtitleVerticalPosition:
        preview.verticalPosition = normalized.toInt();
        break;
    case SettingTarget::SubtitleScale:
        preview.scalePercent = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapSharpness:
        preview.bitmapSharpnessPercent = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapShadowCoreSize:
        preview.bitmapShadowCoreSize = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapShadowCoreGrow:
        preview.bitmapShadowCoreGrow = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapShadowCoreOpacity:
        preview.bitmapShadowCoreOpacityPercent = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadSize:
        preview.bitmapShadowSpreadSize = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadGrow:
        preview.bitmapShadowSpreadGrow = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadX:
        preview.bitmapShadowSpreadX = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadY:
        preview.bitmapShadowSpreadY = normalized.toInt();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadOpacity:
        preview.bitmapShadowSpreadOpacityPercent = normalized.toInt();
        break;
    case SettingTarget::SubtitleHdrBrightness:
        preview.hdrBrightnessPercent = normalized.toInt();
        break;
    default:
        return;
    }
    m_player->previewSubtitlePreferences(preview);
}

void SettingsController::setNightModeEnabled(bool enabled)
{
    setValue(QStringLiteral("settings/nightMode"), enabled);
}
void SettingsController::setAudioDelayMs(int delayMs)
{
    setValue(QStringLiteral("settings/audioDelayMs"), delayMs);
}
void SettingsController::setUiScalePercent(int percent)
{
    setValue(QStringLiteral("appearance/uiScalePercent"), percent);
}
void SettingsController::setSubtitleLanguageIndex(int index)
{
    loadSubtitleLanguages();
    if (index >= 0 && index < m_subtitleLanguageCodes.size())
        setValue(QStringLiteral("subtitles/language"), m_subtitleLanguageCodes.at(index));
}

void SettingsController::resetSubtitleAppearance()
{
    QVariantMap defaults;
    for (const SettingSpec& spec : settingSpecs()) {
        if (spec.persisted && QLatin1String(spec.group) == QLatin1String("Subtitle Appearance"))
            defaults.insert(keyString(spec), settingDefaultValue(spec));
    }
    submitValues(std::move(defaults), ChangeOrigin::User);
}

void SettingsController::applySchemaValue(const SettingSpec& spec, const QVariant& value, bool apply)
{
    switch (spec.target) {
    case SettingTarget::External:
        break;
    case SettingTarget::Locale:
    case SettingTarget::LatencyGuard:
    case SettingTarget::LatencyOverlay:
        break;
    case SettingTarget::AudioLanguage:
        m_subtitlePreferences.audioLanguage = value.toString();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::NightMode:
        m_nightModeEnabled = value.toBool();
        if (apply && m_player)
            m_player->setNightModeEnabled(m_nightModeEnabled);
        break;
    case SettingTarget::RemoteControlTargetEnabled:
        m_remoteControlTargetEnabled = value.toBool();
        if (apply || !m_localSettingsLoaded)
            emit remoteControlTargetEnabledChanged(m_remoteControlTargetEnabled);
        break;
    case SettingTarget::ToneMappingVisualization:
        m_toneMappingVisualizationEnabled = value.toBool();
        if (apply && m_player)
            m_player->setToneMappingVisualizationEnabled(m_toneMappingVisualizationEnabled);
        break;
    case SettingTarget::MaxStreamingHeight:
        m_maxStreamingHeight = value.toInt();
        if (apply)
            applyPlaybackPreferences();
        break;
    case SettingTarget::ManualStreamingBitrate:
        m_manualStreamingBitrate = value.toBool();
        if (apply)
            applyPlaybackPreferences();
        break;
    case SettingTarget::MaxStreamingBitrate:
        m_maxStreamingBitrateMbps = value.toInt();
        if (apply)
            applyPlaybackPreferences();
        break;
    case SettingTarget::UnlimitedLocalBitrate:
        m_unlimitedLocalBitrate = value.toBool();
        if (apply)
            applyPlaybackPreferences();
        break;
    case SettingTarget::PreferRemux:
        m_preferRemux = value.toBool();
        if (apply)
            applyPlaybackPreferences();
        break;
    case SettingTarget::ForwardCacheSize:
        m_forwardCacheSizeMiB = value.toInt();
        if (apply && m_player)
            m_player->setForwardCacheSizeMiB(m_forwardCacheSizeMiB);
        break;
    case SettingTarget::PlayerVolumeSlider:
        break;
    case SettingTarget::AudioDelay:
        m_audioDelayMs = value.toInt();
        if (apply)
            applyAudioDelayToPlayer();
        break;
    case SettingTarget::AudioOutput:
        m_audioOutputMode = value.toString();
        if (apply && m_player)
            m_player->setAudioOutputMode(m_audioOutputMode);
        break;
    case SettingTarget::VideoOutputMode:
        m_videoOutputMode = value.toString();
        if (m_player && (apply || !m_localSettingsLoaded))
            m_player->setDirectVideoOutput(m_videoOutputMode == QLatin1String("direct"));
        break;
    case SettingTarget::SoftwareRenderer:
        if (m_player && (apply || !m_localSettingsLoaded))
            m_player->setSoftwareRenderer(value.toString().toLatin1());
        break;
    case SettingTarget::AutoAdjustRenderQuality:
        m_autoAdjustRenderQuality = value.toBool();
        break;
    case SettingTarget::RenderQuality:
        m_renderQuality = value.toString();
        if (m_player && (apply || !m_localSettingsLoaded))
            m_player->setRenderQuality(MpvOptionProfile::renderQualityFromName(m_renderQuality));
        break;
    case SettingTarget::HardwareDecoding:
        if (m_player && (apply || !m_localSettingsLoaded))
            m_player->setHardwareDecoding(value.toBool());
        break;
    case SettingTarget::HdrOutputMode:
        if (m_player && (apply || !m_localSettingsLoaded))
            m_player->setHdrOutputPreference(value.toString());
        // The mpv side of this applies immediately; the swapchain side cannot,
        // because Qt fixes a window's format when the window is created. Keep
        // it somewhere readable before the next window exists.
        if (apply || !m_localSettingsLoaded)
            RenderTargetPolicy::rememberPreference(RenderTargetPolicy::preferenceFromName(value.toString()));
        break;
    case SettingTarget::GraphicsApi:
        // Qt fixes the scene graph's backend for the life of the process, so
        // like the swapchain format this is recorded for the next launch and
        // read back before QGuiApplication exists.
        if (apply || !m_localSettingsLoaded)
            RenderTargetPolicy::rememberGraphicsApi(RenderTargetPolicy::graphicsApiFromName(value.toString()));
        break;
    case SettingTarget::HdrPeakBrightness:
        if (m_player && (apply || !m_localSettingsLoaded))
            m_player->setHdrPeakNits(value.toInt());
        break;
    case SettingTarget::UiScale:
        m_uiScalePercent = value.toInt();
        break;
    case SettingTarget::AutomaticUpdates:
        // Announced even on the initial load, which is what starts the first
        // check: nothing checks for updates until settings have said it may.
        if (apply || !m_localSettingsLoaded)
            emit automaticUpdatesChanged(value.toBool());
        break;
    case SettingTarget::ArtworkFormat:
        m_artworkFormat = value.toString();
        if (apply)
            applyArtworkEncoding();
        break;
    case SettingTarget::ArtworkWebpQuality:
        m_artworkWebpQuality = value.toInt();
        if (apply)
            applyArtworkEncoding();
        break;
    case SettingTarget::ArtworkJpegQuality:
        m_artworkJpegQuality = value.toInt();
        if (apply)
            applyArtworkEncoding();
        break;
    case SettingTarget::AudioTrackMode:
        m_subtitlePreferences.audioMode = value.toString();
        if (apply) {
            applySubtitlePreferencesToPlayer();
        }
        break;
    case SettingTarget::RememberSeriesAudioTrack:
        break;
    case SettingTarget::SubtitleLanguage:
        m_subtitlePreferences.language = value.toString();
        if (apply) {
            applySubtitlePreferencesToPlayer();
        }
        break;
    case SettingTarget::SubtitleMode:
        m_subtitlePreferences.mode = value.toString();
        if (apply) {
            applySubtitlePreferencesToPlayer();
        }
        break;
    case SettingTarget::SubtitleStyling:
        m_subtitlePreferences.styling = value.toString();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleTextWeight:
        m_subtitlePreferences.textWeight = value.toString();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleFont:
        m_subtitlePreferences.font = value.toString();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleTextColor:
        m_subtitlePreferences.textColor = value.toString();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleTextColorOverride:
        m_subtitlePreferences.overrideTextColor = value.toBool();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleDropShadow:
        m_subtitlePreferences.dropShadow = value.toString();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleTextBackground:
        m_subtitlePreferences.textBackground = value.toString();
        break;
    case SettingTarget::SubtitleVerticalPosition:
        m_subtitlePreferences.verticalPosition = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleScale:
        m_subtitlePreferences.scalePercent = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitlePositionAndSizeOverride:
        m_subtitlePreferences.alwaysOverridePositionAndSize = value.toBool();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapSharpness:
        m_subtitlePreferences.bitmapSharpnessPercent = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleRecolorImages:
        m_subtitlePreferences.recolorImageSubtitles = value.toBool();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowEnabled:
        m_subtitlePreferences.bitmapShadowEnabled = value.toBool();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowCoreSize:
        m_subtitlePreferences.bitmapShadowCoreSize = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowCoreGrow:
        m_subtitlePreferences.bitmapShadowCoreGrow = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowCoreOpacity:
        m_subtitlePreferences.bitmapShadowCoreOpacityPercent = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadEnabled:
        m_subtitlePreferences.bitmapShadowSpreadEnabled = value.toBool();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadSize:
        m_subtitlePreferences.bitmapShadowSpreadSize = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadGrow:
        m_subtitlePreferences.bitmapShadowSpreadGrow = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadX:
        m_subtitlePreferences.bitmapShadowSpreadX = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadY:
        m_subtitlePreferences.bitmapShadowSpreadY = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowSpreadOpacity:
        m_subtitlePreferences.bitmapShadowSpreadOpacityPercent = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleBitmapShadowDither:
        m_subtitlePreferences.bitmapShadowDither = value.toBool();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleAllowInBlackBars:
        m_subtitlePreferences.allowSubtitlesInBlackBars = value.toBool();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::SubtitleHdrBrightness:
        m_subtitlePreferences.hdrBrightnessPercent = value.toInt();
        if (apply)
            applySubtitlePreferencesToPlayer();
        break;
    case SettingTarget::RedButton:
        m_redButtonAction = value.toString();
        break;
    case SettingTarget::GreenButton:
        m_greenButtonAction = value.toString();
        break;
    case SettingTarget::YellowButton:
        m_yellowButtonAction = value.toString();
        break;
    case SettingTarget::BlueButton:
        m_blueButtonAction = value.toString();
        break;
    case SettingTarget::MpvConfigMode:
        m_mpvConfigMode = value.toString();
        if (apply)
            applyMpvConfigPolicy();
        break;
    case SettingTarget::MpvConfigDirectory:
        m_mpvConfigDirectory = value.toString();
        if (apply)
            applyMpvConfigPolicy();
        break;
    }
}

void SettingsController::emitBatchSignals(const QVariantMap& values)
{
    bool subtitle = false;
    bool buttons = false;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        const auto& spec = *findSettingSpec(it.key());
        switch (spec.target) {
        case SettingTarget::NightMode:
            emit nightModeChanged();
            break;
        case SettingTarget::RemoteControlTargetEnabled:
            emit remoteControlSettingsChanged();
            break;
        case SettingTarget::AudioDelay:
            emit audioDelayChanged();
            break;
        case SettingTarget::UiScale:
            emit appearanceChanged();
            break;
        default:
            break;
        }
        subtitle |= it.key().startsWith(QStringLiteral("subtitles/"));
        buttons |= QLatin1String(spec.group) == QLatin1String("Remote buttons");
    }
    if (subtitle)
        emit subtitleSettingsChanged();
    if (buttons)
        emit buttonRemapChanged();
}

void SettingsController::applyArtworkEncoding()
{
    if (!m_artwork || m_batchEffects)
        return;
    // "auto" is resolved here rather than in the schema so the stored value
    // stays portable: the same profile restored on a TV and on a desktop asks
    // each for the format that suits it.
    const QString format = m_artworkFormat == QLatin1String("auto")
        ? QString::fromLatin1(platformDefaultArtworkFormat())
        : m_artworkFormat;
    m_artwork->setArtworkEncoding(format, m_artworkWebpQuality, m_artworkJpegQuality);
}

void SettingsController::applyPlaybackPreferences()
{
    if (m_batchEffects)
        return;
    const qint64 manualBitrate
        = m_manualStreamingBitrate ? static_cast<qint64>(m_maxStreamingBitrateMbps) * 1'000'000 : 0;
    emit playbackPreferencesChanged(manualBitrate, m_unlimitedLocalBitrate, m_preferRemux, m_maxStreamingHeight);
}

void SettingsController::applyAudioDelayToPlayer()
{
    if (!m_player)
        return;
    m_player->setAudioDelayMs(qBound(-2000, m_automaticAudioDelayMs + m_audioDelayMs, 2000));
}

void SettingsController::updateAudioOutputRoute(const QString& output, int displayLatencyMs, int outputLatencyMs)
{
    const QString normalizedOutput = normalizedPlatformAudioRoute(output);
    const int automaticDelay = platformAutomaticAudioDelayMs(normalizedOutput, displayLatencyMs, outputLatencyMs);
    const bool outputChanged = normalizedOutput != m_currentAudioOutput;
    const bool automaticDelayChanged = automaticDelay != m_automaticAudioDelayMs;
    const bool displayLatencyChanged = displayLatencyMs != m_displayLatencyMs;
    if (!outputChanged && !automaticDelayChanged && !displayLatencyChanged)
        return;

    qInfo() << "app: audio output" << normalizedOutput << "display latency" << displayLatencyMs << "ms; output latency"
            << outputLatencyMs << "ms; automatic delay" << automaticDelay << "ms";

    m_audioOutputLoadGeneration.invalidate();
    m_currentAudioOutput = normalizedOutput;
    m_automaticAudioDelayMs = automaticDelay;
    m_displayLatencyMs = displayLatencyMs;
    if (outputChanged) {
        m_audioDelayMs = 0;
        m_values.insert(QStringLiteral("settings/audioDelayMs"), m_audioDelayMs);
    }
    applyAudioDelayToPlayer();

    emit audioOutputDeviceChanged();
    if (outputChanged) {
        emit settingChanged(QStringLiteral("settings/audioDelayMs"));
        emit settingsValuesChanged();
        emit audioDelayChanged();
        if (m_localSettingsLoaded && platformUsesPerOutputAudioDelay())
            loadCurrentAudioDelay();
    }
}

void SettingsController::loadCurrentAudioDelay()
{
    if (!platformUsesPerOutputAudioDelay())
        return;
    const QString output = normalizedPlatformAudioRoute(m_currentAudioOutput);
    const auto token = m_audioOutputLoadGeneration.next();
    Async::runLatest(
        this, m_database->loadSettingAsync(platformAudioDelayStorageKey(output), QStringLiteral("0")),
        m_audioOutputLoadGeneration, token,
        [this, output](const QString& stored) {
            const SettingSpec& spec = specForKey("settings/audioDelayMs");
            applyLoadedAudioDelay(output, normalizedSettingValue(spec, stored).toInt());
        },
        [](const std::exception_ptr& error) {
            qWarning() << "app: failed to load per-output audio delay trim:" << exceptionMessage(error);
        },
        "load per-output audio delay trim");
}

void SettingsController::applyLoadedAudioDelay(const QString& output, int delayMs)
{
    if (normalizedPlatformAudioRoute(output) != m_currentAudioOutput)
        return;

    m_audioDelayMs = delayMs;
    m_values.insert(QStringLiteral("settings/audioDelayMs"), delayMs);
    applyAudioDelayToPlayer();
    qInfo() << "app: loaded audio delay trim for" << m_currentAudioOutput << delayMs << "ms; automatic"
            << m_automaticAudioDelayMs << "ms; effective"
            << qBound(-2000, m_automaticAudioDelayMs + m_audioDelayMs, 2000) << "ms";
    emit settingChanged(QStringLiteral("settings/audioDelayMs"));
    emit settingsValuesChanged();
    emit audioDelayChanged();
}

void SettingsController::applyMpvConfigPolicy()
{
    const MpvConfigPolicy policy = validatedPlatformMpvConfigPolicy(m_mpvConfigMode, m_mpvConfigDirectory);
    if (policy.valid && m_player)
        m_player->setMpvConfigPolicy(policy);
}

void SettingsController::applySubtitlePreferencesToPlayer()
{
    if (m_player && !m_batchEffects)
        m_player->setSubtitlePreferences(m_subtitlePreferences);
}

} // namespace Spool
