#pragma once

#include <QChronoTimer>
#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <functional>

class QQuickWindow;
class QQuickItem;

namespace JellyfinNative {
class InputLatencyMonitor;
class LibraryListModel;
class RouterController;

// The handful of things the benchmark drives in the app, handed in so the
// diagnostics layer never names the composition root.
struct RenderBenchmarkHooks {
    LibraryListModel *libraries = nullptr;
    std::function<void(int index)> openLibrary;
    std::function<int()> outstandingArtworkRequests;
    // decodeMsTotal, decodedPixelsTotal and decodedImagesTotal, as the
    // artwork service counts them.
    std::function<QVariantMap()> artworkDecodeTotals;
    // Identifies the implementation actually serving the UI, not merely a
    // registered module. Update this when the JS application path is wired.
    QString providerId;
    QString providerRuntime;
    // Requests application memory-pressure eviction. This does not flush Qt's
    // disk/AOT caches, the OS page cache, or necessarily every resident page.
    std::function<void()> forceColdCaches;
};

// Walks the app through a scripted set of route switches and writes down what
// each one cost, so "does a page still appear in one frame" is a number in CI
// rather than an impression on somebody's desk.
//
// It drives the real shell -- real models, real artwork, real delegates --
// because the costs worth watching are the ones that only appear when all of
// that is present. Without a server it still measures what a route costs to
// build and paint, which is the part that regresses when a page gains a
// binding it did not need.
//
// Off unless SPOOL_BENCH names a script, so it is inert in a normal run.
class RenderBenchmark final : public QObject {
    Q_OBJECT

public:
    // Returns nullptr when SPOOL_BENCH is unset, which is every ordinary run.
    static RenderBenchmark *createIfRequested(RenderBenchmarkHooks hooks, RouterController *router,
        InputLatencyMonitor *latency, QQuickWindow *window, QObject *parent);

    void start();

private:
    RenderBenchmark(RenderBenchmarkHooks hooks, RouterController *router, InputLatencyMonitor *latency,
        QQuickWindow *window, QObject *parent);

    void step();
    void recordSample();
    // Between steps the app is idle, so any lateness a frame-budget timer
    // shows there is the machine's, not the app's. Measured the same way
    // maxGapMs is, so the two are directly comparable, and measured beside
    // every sample rather than once, because a shared runner is not equally
    // busy from one minute to the next.
    qint64 budgetNs() const;
    // Opens a named library and walks it, so the numbers come from a real
    // grid full of real artwork rather than an empty page. Scrolling is where
    // a library actually costs something: every screen is a fresh set of
    // delegates and a fresh set of images to decode and upload.
    bool openLibraryNamed(const QString& name);
    QQuickItem *routeStackItem();
    QVariant invokeOnActivePage(const QString& function);
    void applyViewMode();
    void beginScrollWalk();
    void scrollStep();
    void finishScrollStep();
    void beginIdleProbe();
    void finishIdleProbe();
    void finish();

    RenderBenchmarkHooks m_hooks;
    RouterController *m_router = nullptr;
    InputLatencyMonitor *m_latency = nullptr;
    // Offscreen, nothing asks for a frame on its own: no compositor is
    // driving the window and an incubating page paints nothing until it is
    // finished. A transition is only recorded once a frame carrying it has
    // been swapped, so the harness has to keep asking for one.
    QQuickWindow *m_window = nullptr;
    QTimer *m_pump = nullptr;
    QTimer *m_stepDeadline = nullptr;
    int m_pumpIntervalMs = 8;
    QVariantList m_failures;

    // The idle probe is the same kind of timer on the same interval as the
    // transition gap timer, so a pause that would show up as a dropped frame
    // shows up here too, and the two numbers mean the same thing.
    QChronoTimer *m_idleProbe = nullptr;
    qint64 m_idleExpectedNs = 0;
    qint64 m_idleWorstNs = 0;
    QElapsedTimer m_idleClock;
    QVariantList m_idleGaps;

    // Library walk state.
    QString m_libraryName;
    bool m_listMode = false;
    bool m_viewModeApplied = false;
    int m_scrollSteps = 0;
    int m_scrollPosition = 0;
    bool m_scrolling = false;
    qint64 m_scrollStartNs = 0;
    qint64 m_scrollWorstGapNs = 0;
    qint64 m_scrollExpectedGapNs = 0;
    QElapsedTimer m_scrollClock;
    QChronoTimer *m_scrollGapTimer = nullptr;
    QTimer *m_scrollPoll = nullptr;
    QVariantList m_scrollSamples;

    QStringList m_script;
    QString m_outputPath;
    int m_iterations = 3;
    int m_warmup = 1;
    int m_stepTimeoutMs = 8000;
    // How long to sit still between steps. Doubles as the idle-probe window,
    // so the noise floor is sampled over the same span a transition occupies.
    int m_settleMs = 120;
    // Request memory-pressure eviction before each step. Record actual
    // cache-hit classification separately; this is not a fully cold process.
    bool m_forceCold = false;

    int m_position = -1;
    int m_pass = 0;
    bool m_awaitingSample = false;
    QVariantList m_samples;
    QElapsedTimer m_stepTimer;
};

} // namespace JellyfinNative
