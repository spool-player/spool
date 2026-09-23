#include "platform/webos/WebOSAudioSyncPolicy.h"

#include <QRegularExpression>
#include <QStringList>
#include <QtGlobal>

namespace Spool::AudioSyncPolicy {

QString normalizedOutputKey(const QString& output)
{
    QString key = output.trimmed().toLower();
    key.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
    key.remove(QRegularExpression(QStringLiteral("^_+|_+$")));
    return key.isEmpty() ? QStringLiteral("unknown") : key;
}

QString outputDisplayName(const QString& output)
{
    const QString key = normalizedOutputKey(output);
    if (key == QStringLiteral("tv_speaker"))
        return QStringLiteral("TV speakers");
    if (key == QStringLiteral("external_arc"))
        return QStringLiteral("ARC / eARC");
    if (key == QStringLiteral("bt_soundbar"))
        return QStringLiteral("Bluetooth soundbar");
    if (key == QStringLiteral("external_optical"))
        return QStringLiteral("Optical output");
    if (key == QStringLiteral("headphone"))
        return QStringLiteral("Headphones");
    if (key == QStringLiteral("unknown"))
        return QStringLiteral("Current output");

    QStringList words = key.split(QLatin1Char('_'), Qt::SkipEmptyParts);
    for (QString& word : words) {
        if (!word.isEmpty())
            word[0] = word[0].toUpper();
    }
    return words.join(QLatin1Char(' '));
}

QString delayStorageKey(const QString& output)
{
    return QStringLiteral("settings/webosAudioDelayMs/") + normalizedOutputKey(output);
}

int automaticBaseDelayMs(const QString& output, int displayLatencyMs, int outputLatencyMs)
{
    const QString key = normalizedOutputKey(output);

    // The TV's Audio Latency Time control excludes Bluetooth codec, link, and
    // receiver latency. The measured end-to-end correction is the useful
    // value for this route.
    if (key == QStringLiteral("bt_soundbar"))
        return 20;

    if (displayLatencyMs >= 0 && outputLatencyMs >= 0)
        return qBound(-2000, displayLatencyMs - outputLatencyMs, 2000);

    if (key == QStringLiteral("external_arc"))
        return 97;
    if (key == QStringLiteral("tv_speaker"))
        return 76;
    return 0;
}

}
