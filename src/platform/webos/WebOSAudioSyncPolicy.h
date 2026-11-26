#pragma once

#include <QString>

namespace Spool::AudioSyncPolicy {

struct RouteLatencies {
    int displayMs;
    int outputMs;
};

QString normalizedOutputKey(const QString& output);
QString outputDisplayName(const QString& output);
QString delayStorageKey(const QString& output);
RouteLatencies resolveRouteLatencies(const QString& output, int displayMs, int outputMs, int firmwareMediaMs = -1);
int automaticBaseDelayMs(const QString& output, int displayLatencyMs, int outputLatencyMs);

}
