#include "RenderBenchmark.h"

#include "../app/RouterController.h"
#include "../models/LibraryListModel.h"
#include "InputLatencyMonitor.h"

#include <QChronoTimer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QtGlobal>

#include <utility>

#include <algorithm>
#include <chrono>

namespace JellyfinNative {
namespace {

    // Routes that stand up without a library behind them, so the same script runs
    // on a developer's machine against a real server and in CI against nothing.
    const QStringList kDefaultScript
        = { QStringLiteral("home"), QStringLiteral("search"), QStringLiteral("settings"), QStringLiteral("home"),
              QStringLiteral("libraryGrid"), QStringLiteral("openSourceNotices"), QStringLiteral("home") };

    int envInt(const char *name, int fallback)
    {
        bool ok = false;
        const int value = qEnvironmentVariableIntValue(name, &ok);
        return ok ? value : fallback;
    }

} // namespace

RenderBenchmark *RenderBenchmark::createIfRequested(RenderBenchmarkHooks hooks, RouterController *router,
    InputLatencyMonitor *latency, QQuickWindow *window, QObject *parent)
{
    const QString script = qEnvironmentVariable("SPOOL_BENCH").trimmed();
    if (script.isEmpty())
        return nullptr;
    auto *benchmark = new RenderBenchmark(std::move(hooks), router, latency, window, parent);
    // "library" walks one library's grid instead of the route set: open it,
    // then page down through it waiting for the artwork on each screen. That
    // is the part of this application that costs the most on a television and
    // the part the route walk cannot see, because a route switch reveals a
    // page that is already built and already holding its pictures.
    if (script == QStringLiteral("library")) {
        benchmark->m_script = { QStringLiteral("libraryGrid") };
        if (benchmark->m_scrollSteps <= 0)
            benchmark->m_scrollSteps = 12;
        if (benchmark->m_libraryName.isEmpty())
            benchmark->m_libraryName = QStringLiteral("Movies");
    } else if (script != QStringLiteral("routes")) {
        benchmark->m_script = script.split(QLatin1Char(','), Qt::SkipEmptyParts);
    }
    return benchmark;
}

RenderBenchmark::RenderBenchmark(RenderBenchmarkHooks hooks, RouterController *router, InputLatencyMonitor *latency,
    QQuickWindow *window, QObject *parent)
    : QObject(parent)
    , m_hooks(std::move(hooks))
    , m_router(router)
    , m_latency(latency)
    , m_window(window)
    , m_script(kDefaultScript)
    , m_outputPath(qEnvironmentVariable("SPOOL_BENCH_OUT"))
    , m_iterations(envInt("SPOOL_BENCH_ITERATIONS", 3))
    , m_warmup(envInt("SPOOL_BENCH_WARMUP", 1))
    , m_stepTimeoutMs(envInt("SPOOL_BENCH_TIMEOUT_MS", 8000))
    , m_settleMs(envInt("SPOOL_BENCH_SETTLE_MS", 120))
    , m_libraryName(qEnvironmentVariable("SPOOL_BENCH_LIBRARY").trimmed())
    , m_listMode(qEnvironmentVariableIntValue("SPOOL_BENCH_LIST_MODE") != 0)
    , m_scrollSteps(envInt("SPOOL_BENCH_SCROLL_STEPS", 0))
    , m_forceCold(qEnvironmentVariableIntValue("SPOOL_BENCH_COLD") != 0)
{
    // Every route switch ends by publishing what it cost. That is the signal
    // the walk advances on, so the harness measures the same transition the
    // diagnostics overlay shows rather than a stopwatch of its own.
    connect(m_latency, &InputLatencyMonitor::routeSampleChanged, this, [this] {
        if (m_awaitingSample)
            recordSample();
    });

    m_pump = new QTimer(this);
    m_pump->setInterval(8);
    connect(m_pump, &QTimer::timeout, this, [this] {
        if (m_window)
            m_window->requestUpdate();
    });

    m_scrollGapTimer = new QChronoTimer(this);
    connect(m_scrollGapTimer, &QChronoTimer::timeout, this, [this] {
        const qint64 nowNs = m_scrollClock.nsecsElapsed();
        m_scrollWorstGapNs = std::max(m_scrollWorstGapNs, std::max<qint64>(0, nowNs - m_scrollExpectedGapNs));
        m_scrollExpectedGapNs = nowNs + budgetNs();
    });

    // A screen of a library is settled when every image it asked for has
    // arrived. Polling the artwork service is what makes that observable
    // without the harness knowing anything about the page it is driving.
    m_scrollPoll = new QTimer(this);
    m_scrollPoll->setInterval(4);
    connect(m_scrollPoll, &QTimer::timeout, this, [this] {
        if (!m_scrolling)
            return;
        const int outstanding = m_hooks.outstandingArtworkRequests ? m_hooks.outstandingArtworkRequests() : 0;
        if (outstanding == 0 || m_scrollClock.elapsed() > m_stepTimeoutMs)
            finishScrollStep();
    });

    m_idleProbe = new QChronoTimer(this);
    connect(m_idleProbe, &QChronoTimer::timeout, this, [this] {
        const qint64 nowNs = m_idleClock.nsecsElapsed();
        m_idleWorstNs = std::max(m_idleWorstNs, std::max<qint64>(0, nowNs - m_idleExpectedNs));
        m_idleExpectedNs = nowNs + budgetNs();
    });
}

qint64 RenderBenchmark::budgetNs() const
{
    const double budgetMs = m_latency ? m_latency->frameBudgetMs() : 16.67;
    return static_cast<qint64>((budgetMs > 0.0 ? budgetMs : 16.67) * 1000000.0);
}

bool RenderBenchmark::openLibraryNamed(const QString& name)
{
    LibraryListModel *libraries = m_hooks.libraries;
    if (!libraries || !m_hooks.openLibrary) {
        qWarning() << "render benchmark: no library list to search";
        return false;
    }
    for (int index = 0; index < libraries->count(); ++index) {
        if (libraries->libraryAt(index).name.compare(name, Qt::CaseInsensitive) == 0) {
            qInfo() << "render benchmark: opening library" << name;
            m_hooks.openLibrary(index);
            return true;
        }
    }
    QStringList available;
    for (int index = 0; index < libraries->count(); ++index)
        available.append(libraries->libraryAt(index).name);
    qWarning() << "render benchmark: no library named" << name << "- have" << available;
    return false;
}

QQuickItem *RenderBenchmark::routeStackItem()
{
    if (!m_window)
        return nullptr;
    // AppShell already names the instance; this is not a hook added for the
    // benchmark, just the one handle it needs.
    return m_window->findChild<QQuickItem *>(QStringLiteral("shellRouteStack"));
}

QVariant RenderBenchmark::invokeOnActivePage(const QString& function)
{
    QQuickItem *stack = routeStackItem();
    if (!stack)
        return {};
    QVariant result;
    // The QML function declares its parameter as `string`, so the meta-method
    // takes a QString: handing it a QVariant silently matches nothing.
    QMetaObject::invokeMethod(
        stack, "invokeOnActivePage", Qt::DirectConnection, Q_RETURN_ARG(QVariant, result), Q_ARG(QString, function));
    return result;
}

// Toggling is a flip rather than a set, so it has to happen exactly once.
void RenderBenchmark::applyViewMode()
{
    if (!m_listMode || m_viewModeApplied)
        return;
    qInfo() << "render benchmark: switching to list mode";
    invokeOnActivePage(QStringLiteral("toggleViewMode"));
    m_viewModeApplied = true;
}

void RenderBenchmark::beginScrollWalk()
{
    qInfo() << "render benchmark: walking" << m_libraryName << (m_listMode ? "as a list" : "as a grid");
    applyViewMode();
    m_scrollPosition = 0;
    m_pump->start();
    // Times how long the library has had to arrive, which is what the first
    // step's patience is measured against.
    m_scrollClock.start();
    QTimer::singleShot(m_settleMs * 4, this, [this] { scrollStep(); });
}

void RenderBenchmark::scrollStep()
{
    if (m_scrollPosition >= m_scrollSteps) {
        m_pump->stop();
        finish();
        return;
    }
    const QVariant moved = invokeOnActivePage(QStringLiteral("scrollPageDown"));
    if (!moved.toBool()) {
        // Before the first screen, "nowhere to scroll" means the library has
        // not finished arriving rather than that it is one screen long. Wait
        // for it; afterwards the same answer really is the end.
        if (m_scrollPosition == 0 && m_scrollClock.elapsed() < m_stepTimeoutMs) {
            QTimer::singleShot(100, this, [this] { scrollStep(); });
            return;
        }
        qInfo() << "render benchmark: reached the end of the library after" << m_scrollPosition << "screens";
        m_pump->stop();
        finish();
        return;
    }
    ++m_scrollPosition;
    m_scrollWorstGapNs = 0;
    m_scrollClock.start();
    m_scrollExpectedGapNs = budgetNs();
    m_scrollGapTimer->setInterval(std::chrono::nanoseconds(budgetNs()));
    m_scrollGapTimer->start();
    m_scrolling = true;
    m_scrollPoll->start();
}

void RenderBenchmark::finishScrollStep()
{
    m_scrolling = false;
    m_scrollPoll->stop();
    m_scrollGapTimer->stop();
    QVariantMap sample;
    sample.insert(QStringLiteral("screen"), m_scrollPosition);
    if (m_hooks.artworkDecodeTotals)
        sample.insert(m_hooks.artworkDecodeTotals());
    sample.insert(QStringLiteral("settleMs"), static_cast<double>(m_scrollClock.nsecsElapsed()) / 1000000.0);
    sample.insert(QStringLiteral("maxGapMs"), static_cast<double>(m_scrollWorstGapNs) / 1000000.0);
    sample.insert(QStringLiteral("frameBudgetMs"), m_latency ? m_latency->frameBudgetMs() : 16.67);
    m_scrollSamples.append(sample);
    QTimer::singleShot(m_settleMs, this, [this] { scrollStep(); });
}

void RenderBenchmark::beginIdleProbe()
{
    m_idleWorstNs = 0;
    m_idleClock.start();
    m_idleExpectedNs = budgetNs();
    m_idleProbe->setInterval(std::chrono::nanoseconds(budgetNs()));
    m_idleProbe->start();
}

void RenderBenchmark::finishIdleProbe()
{
    if (!m_idleProbe->isActive())
        return;
    m_idleProbe->stop();
    // Warmup passes are not recorded, and neither is their noise: the two
    // have to describe the same stretch of the run to be comparable.
    if (m_pass >= 0)
        m_idleGaps.append(static_cast<double>(m_idleWorstNs) / 1000000.0);
}

void RenderBenchmark::start()
{
    if (m_script.isEmpty()) {
        finish();
        return;
    }
    // The timeline is off unless somebody asked for it, which is right for a
    // normal run and wrong here: measuring is the entire job.
    m_latency->setEnabled(true);
    qInfo() << "render benchmark: script" << m_script << "iterations" << m_iterations << "warmup" << m_warmup;
    // Measurement refuses to run against a window nobody can see, which is
    // easy to arrive at by accident on a headless machine. Say so plainly
    // rather than producing an empty report.
    if (m_window) {
        qInfo() << "render benchmark: window visible=" << m_window->isVisible() << "exposed=" << m_window->isExposed();
    }
    // One pass before the first recorded one, so a cold cache and a
    // just-in-time compile do not get counted as the steady state.
    m_pass = -m_warmup;
    m_position = -1;
    if (m_libraryName.isEmpty()) {
        QTimer::singleShot(0, this, [this] { step(); });
        return;
    }

    // The library list arrives from the server some time after the window
    // does, so asking for one at startup finds an empty list. Wait for it
    // rather than reporting that the library does not exist.
    auto *waiter = new QTimer(this);
    waiter->setInterval(50);
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + m_stepTimeoutMs;
    connect(waiter, &QTimer::timeout, this, [this, waiter, deadline] {
        LibraryListModel *libraries = m_hooks.libraries;
        const bool ready = libraries && libraries->count() > 0;
        if (!ready && QDateTime::currentMSecsSinceEpoch() < deadline)
            return;
        waiter->stop();
        waiter->deleteLater();
        if (!ready || !openLibraryNamed(m_libraryName)) {
            finish();
            return;
        }
        // Switch the view before the walk rather than after it. The scroll
        // walk used to be the only thing that ran in list mode, which left
        // every route switch measured as a grid however the run was asked
        // for -- so a list-mode comparison silently compared two grids.
        applyViewMode();
        QTimer::singleShot(0, this, [this] { step(); });
    });
    waiter->start();
}

void RenderBenchmark::step()
{
    ++m_position;
    if (m_position >= m_script.size()) {
        m_position = 0;
        if (++m_pass >= m_iterations) {
            if (m_scrollSteps > 0 && m_scrollSamples.isEmpty()) {
                beginScrollWalk();
                return;
            }
            finish();
            return;
        }
    }

    const QString route = m_script.at(m_position);
    // Replacing the route you are already on is not a transition, so nothing
    // would report and the walk would sit waiting for a frame that is never
    // coming. Step over it rather than time out.
    if (m_router->route() == route) {
        QTimer::singleShot(0, this, [this] { step(); });
        return;
    }

    if (m_forceCold && m_hooks.forceColdCaches)
        m_hooks.forceColdCaches();

    m_awaitingSample = true;
    m_stepTimer.start();
    m_pump->start();
    m_router->replace(route);
    if (m_window)
        m_window->requestUpdate();

    // A route that never reports is a failure worth seeing rather than a
    // hang: give up on it and carry on with the rest of the walk.
    QTimer::singleShot(m_stepTimeoutMs, this, [this, route] {
        if (!m_awaitingSample)
            return;
        qWarning() << "render benchmark: route" << route << "never reported a frame";
        m_awaitingSample = false;
        m_pump->stop();
        QTimer::singleShot(0, this, [this] { step(); });
    });
}

void RenderBenchmark::recordSample()
{
    m_awaitingSample = false;
    m_pump->stop();
    QVariantMap metrics = m_latency->lastRouteMetrics();
    if (!metrics.isEmpty() && m_pass >= 0) {
        metrics.insert(QStringLiteral("pass"), m_pass);
        metrics.insert(QStringLiteral("step"), m_position);
        m_samples.append(metrics);
    }
    // Let the frame settle before asking for the next route, so one
    // transition's tail is not charged to the next one's head. The app has
    // nothing to do in that window, which is exactly when the machine's own
    // scheduling jitter can be measured.
    beginIdleProbe();
    QTimer::singleShot(m_settleMs, this, [this] {
        finishIdleProbe();
        step();
    });
}

void RenderBenchmark::finish()
{
    QJsonArray samples;
    for (const QVariant& sample : std::as_const(m_samples))
        samples.append(QJsonObject::fromVariantMap(sample.toMap()));

    QJsonObject report;
    report.insert(QStringLiteral("recordedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    report.insert(QStringLiteral("script"), QJsonArray::fromStringList(m_script));
    report.insert(QStringLiteral("iterations"), m_iterations);
    report.insert(QStringLiteral("cold"), m_forceCold);
    report.insert(QStringLiteral("frameBudgetMs"), m_latency ? m_latency->frameBudgetMs() : 16.67);
    // Which paint path produced these numbers. It decides whether the frame
    // gaps mean anything: the software backend rasterises on the CPU, so on a
    // small runner a gap is mostly llvmpipe's throughput rather than anything
    // this application did.
    report.insert(QStringLiteral("quickBackend"), qEnvironmentVariable("QT_QUICK_BACKEND"));
    // What a frame-budget timer drifted by while the app was doing nothing.
    // A transition gap only means something when it is worse than this.
    QJsonArray idleGaps;
    for (const QVariant& gap : std::as_const(m_idleGaps))
        idleGaps.append(gap.toDouble());
    report.insert(QStringLiteral("idleGapsMs"), idleGaps);
    report.insert(QStringLiteral("samples"), samples);
    QJsonArray scrollSamples;
    for (const QVariant& sample : std::as_const(m_scrollSamples))
        scrollSamples.append(QJsonObject::fromVariantMap(sample.toMap()));
    report.insert(QStringLiteral("scrollSamples"), scrollSamples);
    report.insert(QStringLiteral("library"), m_libraryName);
    report.insert(QStringLiteral("listMode"), m_listMode);

    const QByteArray json = QJsonDocument(report).toJson(QJsonDocument::Indented);
    if (m_outputPath.isEmpty()) {
        qInfo().noquote() << json;
    } else {
        QDir().mkpath(QFileInfo(m_outputPath).absolutePath());
        QFile file(m_outputPath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(json);
            qInfo() << "render benchmark: wrote" << m_samples.size() << "samples to" << m_outputPath;
        } else {
            qWarning() << "render benchmark: could not write" << m_outputPath;
        }
    }
    QCoreApplication::exit(m_samples.isEmpty() && m_scrollSamples.isEmpty() ? 1 : 0);
}

} // namespace JellyfinNative
