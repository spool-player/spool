#include "platform/webos/WebOSAudioSyncPolicy.h"

#include "TestMain.h"

#include <QCoreApplication>
#include <QDebug>

#include <cstdlib>

using namespace Spool;

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    qCritical() << message;
    std::exit(EXIT_FAILURE);
}

}

SPOOL_TEST_MAIN("audio-sync-policy")
{
    QCoreApplication app(argc, argv);

    require(AudioSyncPolicy::normalizedOutputKey(QStringLiteral(" External ARC! ")) == QStringLiteral("external_arc"),
        "output key normalization failed");
    require(AudioSyncPolicy::delayStorageKey(QStringLiteral("bt_soundbar"))
            == QStringLiteral("settings/webosAudioDelayMs/bt_soundbar"),
        "per-output storage key was incorrect");
    require(AudioSyncPolicy::outputDisplayName(QStringLiteral("external_arc")) == QStringLiteral("ARC / eARC"),
        "ARC display name was incorrect");
    require(AudioSyncPolicy::automaticBaseDelayMs(QStringLiteral("external_arc"), 150, 53) == 97,
        "ARC automatic delay was incorrect");
    require(AudioSyncPolicy::automaticBaseDelayMs(QStringLiteral("tv_speaker"), 150, 74) == 76,
        "TV speaker automatic delay was incorrect");
    require(AudioSyncPolicy::automaticBaseDelayMs(QStringLiteral("bt_soundbar"), 150, 42) == 20,
        "Bluetooth must use the calibrated end-to-end delay");
    require(AudioSyncPolicy::automaticBaseDelayMs(QStringLiteral("external_arc"), -1, -1) == 97,
        "ARC fallback delay was incorrect");
    const auto inactiveArc = AudioSyncPolicy::resolveRouteLatencies(QStringLiteral("external_arc"), 0, 0);
    require(inactiveArc.displayMs == 150 && inactiveArc.outputMs == 53,
        "Zero-filled ARC readings need the calibrated fallback");
    const auto headphones = AudioSyncPolicy::resolveRouteLatencies(QStringLiteral("headphone"), 0, 0, 60);
    require(headphones.displayMs == 150 && headphones.outputMs == 60,
        "Firmware output latency should be used when available");

    return EXIT_SUCCESS;
}
