#pragma once

#include "platform/PlatformPerformanceSampler.h"

#include <QObject>
#include <QTimer>

#include <functional>

namespace Spool {

class SystemPerformanceMonitor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY metricsChanged)
    Q_PROPERTY(bool systemStatsAvailable READ systemStatsAvailable NOTIFY metricsChanged)
    Q_PROPERTY(bool threadBreakdownAvailable READ threadBreakdownAvailable NOTIFY metricsChanged)
    Q_PROPERTY(bool preciseThreadCpuAvailable READ preciseThreadCpuAvailable NOTIFY metricsChanged)
    Q_PROPERTY(double processCpuPercent READ processCpuPercent NOTIFY metricsChanged)
    Q_PROPERTY(double systemCpuPercent READ systemCpuPercent NOTIFY metricsChanged)
    Q_PROPERTY(double mpvCpuPercent READ mpvCpuPercent NOTIFY metricsChanged)
    Q_PROPERTY(double videoDecodeCpuPercent READ videoDecodeCpuPercent NOTIFY metricsChanged)
    Q_PROPERTY(double audioDecodeCpuPercent READ audioDecodeCpuPercent NOTIFY metricsChanged)
    Q_PROPERTY(double audioOutputCpuPercent READ audioOutputCpuPercent NOTIFY metricsChanged)
    Q_PROPERTY(double loadOne READ loadOne NOTIFY metricsChanged)
    Q_PROPERTY(double loadFive READ loadFive NOTIFY metricsChanged)
    Q_PROPERTY(double loadFifteen READ loadFifteen NOTIFY metricsChanged)
    Q_PROPERTY(qint64 processRssBytes READ processRssBytes NOTIFY metricsChanged)
    Q_PROPERTY(qint64 processAnonymousBytes READ processAnonymousBytes NOTIFY metricsChanged)
    Q_PROPERTY(qint64 systemUsedBytes READ systemUsedBytes NOTIFY metricsChanged)
    Q_PROPERTY(qint64 systemAvailableBytes READ systemAvailableBytes NOTIFY metricsChanged)
    Q_PROPERTY(qint64 systemTotalBytes READ systemTotalBytes NOTIFY metricsChanged)

public:
    explicit SystemPerformanceMonitor(QObject *parent = nullptr);
    void setAudioDecodeCpuTimeProvider(std::function<qint64()> provider);

    bool available() const
    {
        return m_available;
    }
    bool systemStatsAvailable() const
    {
        return m_systemStatsAvailable;
    }
    bool threadBreakdownAvailable() const
    {
        return m_threadBreakdownAvailable;
    }
    bool preciseThreadCpuAvailable() const
    {
        return m_preciseThreadCpuAvailable;
    }
    double processCpuPercent() const
    {
        return m_processCpuPercent;
    }
    double systemCpuPercent() const
    {
        return m_systemCpuPercent;
    }
    double mpvCpuPercent() const
    {
        return m_mpvCpuPercent;
    }
    double videoDecodeCpuPercent() const
    {
        return m_videoDecodeCpuPercent;
    }
    double audioDecodeCpuPercent() const
    {
        return m_audioDecodeCpuPercent;
    }
    double audioOutputCpuPercent() const
    {
        return m_audioOutputCpuPercent;
    }
    double loadOne() const
    {
        return m_loadOne;
    }
    double loadFive() const
    {
        return m_loadFive;
    }
    double loadFifteen() const
    {
        return m_loadFifteen;
    }
    qint64 processRssBytes() const
    {
        return m_processRssBytes;
    }
    qint64 processAnonymousBytes() const
    {
        return m_processAnonymousBytes;
    }
    qint64 systemUsedBytes() const
    {
        return m_systemUsedBytes;
    }
    qint64 systemAvailableBytes() const
    {
        return m_systemAvailableBytes;
    }
    qint64 systemTotalBytes() const
    {
        return m_systemTotalBytes;
    }

signals:
    void metricsChanged();

private:
    void sample();

    PlatformPerformanceSampler m_sampler;
    QTimer m_timer;
    std::function<qint64()> m_audioDecodeCpuTimeProvider;
    bool m_available = false;
    bool m_systemStatsAvailable = false;
    bool m_sampleLogged = false;
    bool m_threadBreakdownAvailable = false;
    bool m_preciseThreadCpuAvailable = false;
    double m_processCpuPercent = 0.0;
    double m_systemCpuPercent = 0.0;
    double m_mpvCpuPercent = 0.0;
    double m_videoDecodeCpuPercent = 0.0;
    double m_audioDecodeCpuPercent = 0.0;
    double m_audioOutputCpuPercent = 0.0;
    double m_loadOne = 0.0;
    double m_loadFive = 0.0;
    double m_loadFifteen = 0.0;
    qint64 m_processRssBytes = 0;
    qint64 m_processAnonymousBytes = 0;
    qint64 m_systemUsedBytes = 0;
    qint64 m_systemAvailableBytes = 0;
    qint64 m_systemTotalBytes = 0;
};

} // namespace Spool
