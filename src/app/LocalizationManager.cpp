#include "LocalizationManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QLocale>
#include <QSettings>

namespace Spool {

namespace {
    constexpr auto kSettingsKey = "i18n/locale";
}

LocalizationManager::LocalizationManager(QObject *parent)
    : QObject(parent)
{
    QSettings settings;
    const QString persisted = settings.value(QLatin1String(kSettingsKey)).toString();
    if (persisted.isEmpty() || persisted == QLatin1String("system")) {
        m_useSystem = true;
        applyLocale(QLocale::system().bcp47Name());
    } else {
        m_useSystem = false;
        applyLocale(persisted);
    }
}

QString LocalizationManager::currentLocale() const
{
    return m_currentLocale;
}

QString LocalizationManager::bcp47Locale() const
{
    return QLocale(m_currentLocale).bcp47Name();
}

QStringList LocalizationManager::availableLocales() const
{
    return { QStringLiteral("system"), QStringLiteral("en-AU"), QStringLiteral("en-GB"), QStringLiteral("en-US") };
}

void LocalizationManager::attachToEngine(QQmlEngine *engine)
{
    m_engine = engine;
    if (m_engine)
        m_engine->setUiLanguage(m_currentLocale);
}

void LocalizationManager::setLocale(const QString& localeTag)
{
    if (localeTag == QLatin1String("system")) {
        useSystemDefault();
        return;
    }
    m_useSystem = false;
    QSettings().setValue(QLatin1String(kSettingsKey), localeTag);
    applyLocale(localeTag);
}

void LocalizationManager::useSystemDefault()
{
    m_useSystem = true;
    QSettings().setValue(QLatin1String(kSettingsKey), QStringLiteral("system"));
    applyLocale(QLocale::system().bcp47Name());
}

QString LocalizationManager::displayNameFor(const QString& localeTag) const
{
    if (localeTag == QLatin1String("system")) {
        const QLocale systemLocale = QLocale::system();
        return QStringLiteral("System (%1)").arg(QLocale::languageToString(systemLocale.language()));
    }
    return QLocale(localeTag).nativeLanguageName();
}

void LocalizationManager::applyLocale(const QString& localeTag)
{
    const QString normalized = localeTag.isEmpty() ? QStringLiteral("en-US") : localeTag;
    QLocale locale(normalized);
    QLocale::setDefault(locale);

    qApp->removeTranslator(&m_translator);
    // The Qt Linguist filename convention used by qt_add_translations is
    // qtfin_<locale>.qm with underscores in the locale, so normalize the
    // BCP-47 dash form first.
    const QString qtName = QString(normalized).replace(QLatin1Char('-'), QLatin1Char('_'));
    const QString resourcePath = QStringLiteral(":/i18n/qtfin_%1.qm").arg(qtName);
    if (QFileInfo::exists(resourcePath) && m_translator.load(resourcePath)) {
        qApp->installTranslator(&m_translator);
    } else {
        // Fall back silently: untranslated text falls through to the source
        // string passed to qsTrId / tr().
        qInfo() << "i18n: no .qm at" << resourcePath << "— using source strings";
    }

    m_currentLocale = normalized;
    if (m_engine)
        m_engine->setUiLanguage(m_currentLocale);
    emit localeChanged();
}

} // namespace Spool
