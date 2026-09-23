#pragma once

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QTranslator>

namespace Spool {

// Owns the active Qt translator and exposes locale switching to QML. The
// strategy follows the Qt-native approach: .qm files compiled from .ts files
// live under :/i18n/qtfin_<locale>.qm; this class loads the matching .qm
// when the user (or the system locale) changes.
//
// Today we ship English variants only, but the manager is BCP-47 friendly so
// we can land more locales without code changes.
class LocalizationManager final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString currentLocale READ currentLocale NOTIFY localeChanged)
    Q_PROPERTY(QStringList availableLocales READ availableLocales CONSTANT)
    Q_PROPERTY(bool useSystemLocale READ useSystemLocale NOTIFY localeChanged)

public:
    explicit LocalizationManager(QObject *parent = nullptr);

    QString currentLocale() const;
    QStringList availableLocales() const;
    bool useSystemLocale() const
    {
        return m_useSystem;
    }

    // BCP-47 form for protocol headers (Accept-Language).
    QString bcp47Locale() const;

    void attachToEngine(QQmlEngine *engine);

    Q_INVOKABLE void setLocale(const QString& localeTag);
    Q_INVOKABLE void useSystemDefault();
    Q_INVOKABLE QString displayNameFor(const QString& localeTag) const;

signals:
    void localeChanged();

private:
    void applyLocale(const QString& localeTag);

    QTranslator m_translator;
    QQmlEngine *m_engine = nullptr;
    QString m_currentLocale;
    bool m_useSystem = true;
};

} // namespace Spool
