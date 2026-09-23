#include "platform/webos/WebOSAudioRoute.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>

#include <atomic>

extern "C" {
#include <alsa/asoundlib.h>
}

namespace Spool {

namespace {
    struct LatencySnapshot {
        int displayLatencyMs = -1;
        int outputLatencyMs = -1;
    };

    int readLastInteger(snd_ctl_t *control, unsigned int numid)
    {
        snd_ctl_elem_id_t *id = nullptr;
        snd_ctl_elem_info_t *info = nullptr;
        snd_ctl_elem_value_t *value = nullptr;
        snd_ctl_elem_id_alloca(&id);
        snd_ctl_elem_info_alloca(&info);
        snd_ctl_elem_value_alloca(&value);
        snd_ctl_elem_id_set_numid(id, numid);
        snd_ctl_elem_info_set_id(info, id);
        if (snd_ctl_elem_info(control, info) < 0)
            return -1;
        snd_ctl_elem_value_set_id(value, id);
        if (snd_ctl_elem_read(control, value) < 0)
            return -1;
        const unsigned int count = snd_ctl_elem_info_get_count(info);
        return count == 0 ? -1 : static_cast<int>(snd_ctl_elem_value_get_integer(value, count - 1));
    }

    LatencySnapshot readLatency()
    {
        LatencySnapshot snapshot;
        snd_ctl_t *control = nullptr;
        const int result = snd_ctl_open(&control, "hw:0", 0);
        if (result < 0) {
            qWarning() << "webOS audio route: snd_ctl_open failed" << snd_strerror(result);
            return snapshot;
        }
        snapshot.displayLatencyMs = readLastInteger(control, 62);
        snapshot.outputLatencyMs = readLastInteger(control, 96);
        snd_ctl_close(control);
        return snapshot;
    }
} // namespace

struct WebOSAudioRoute::PlatformData {
    explicit PlatformData(WebOSAudioRoute *route)
        : owner(route)
    {
    }

    WebOSAudioRoute *owner = nullptr;
    QTimer pollTimer;
    std::atomic_uint64_t generation { 0 };
    QMutex mutex;
    QString pendingOutput;
    LatencySnapshot pendingLatency;
    std::uint64_t appliedGeneration = 0;
};

WebOSAudioRoute::WebOSAudioRoute(QObject *parent)
    : QObject(parent)
    , m_platform(std::make_unique<PlatformData>(this))
{
    m_platform->pollTimer.setInterval(100);
    connect(&m_platform->pollTimer, &QTimer::timeout, this, [this] {
        const std::uint64_t generation = m_platform->generation.load();
        if (generation == m_platform->appliedGeneration)
            return;
        QString output;
        LatencySnapshot latency;
        {
            QMutexLocker locker(&m_platform->mutex);
            output = m_platform->pendingOutput;
            latency = m_platform->pendingLatency;
        }
        m_platform->appliedGeneration = generation;
        if (output.isEmpty())
            return;
        emit routeChanged(output, latency.displayLatencyMs, latency.outputLatencyMs);
        QTimer::singleShot(250, this, [this, output, generation] {
            if (m_platform->generation.load() != generation)
                return;
            const LatencySnapshot settled = readLatency();
            qInfo() << "webOS audio route settled" << output << settled.displayLatencyMs << settled.outputLatencyMs;
            emit routeChanged(output, settled.displayLatencyMs, settled.outputLatencyMs);
        });
    });
    m_platform->pollTimer.start();
}

WebOSAudioRoute::~WebOSAudioRoute() = default;

void WebOSAudioRoute::acceptServicePayload(const QByteArray& payload)
{
    const QJsonDocument document = QJsonDocument::fromJson(payload);
    const QString output
        = document.isObject() ? document.object().value(QStringLiteral("soundOutput")).toString() : QString();
    if (output.isEmpty()) {
        qWarning() << "webOS audio route: invalid service payload";
        return;
    }
    const LatencySnapshot latency = readLatency();
    {
        QMutexLocker locker(&m_platform->mutex);
        m_platform->pendingOutput = output;
        m_platform->pendingLatency = latency;
    }
    ++m_platform->generation;
}

} // namespace Spool
