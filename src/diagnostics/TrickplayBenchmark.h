#pragma once

#include <QByteArray>
#include <QImage>
#include <QQuickItem>
#include <QRect>
#include <QSize>
#include <QVariantMap>
#include <array>
#include <memory>

namespace Spool {

enum class TrickplayBenchFormat { Rgb32, Yuv420 };

struct TrickplayBenchFrame {
    TrickplayBenchFormat format = TrickplayBenchFormat::Rgb32;
    QSize size;
    // Qt RGB output is retained without copying into a second full-image buffer.
    QImage rgbImage;
    // Planes own decoded pixels, or borrow rgbImage for the Qt RGB case.
    // Planar output may include padded final MCU rows; only the visible size
    // (ceil-divided by two for chroma) reaches the GPU.
    std::array<QByteArray, 3> planes;
    std::array<int, 3> strides {};
};

class TrickplayBenchmarkItem final : public QQuickItem {
    Q_OBJECT
public:
    explicit TrickplayBenchmarkItem(QQuickItem *parent = nullptr);
    ~TrickplayBenchmarkItem() override;
    // GUI thread. A new frame pointer uploads pixels; crop-only changes do not.
    void setFrame(std::shared_ptr<const TrickplayBenchFrame> frame, const QRect& crop, quint64 serial);
    void clearFrame(quint64 serial);

signals:
    // One record per requested serial, after the scene graph records its frame.
    // All CPU timings are nanoseconds. GPU duration is explicitly a delayed
    // whole-frame sample, not an isolated upload or preview draw duration.
    void sampleReady(QVariantMap sample);
    // Exact originating request where the backend's frame-slot query lifecycle
    // makes attribution possible (Vulkan). Emitted later; never waits on GPU.
    void gpuSampleReady(QVariantMap sample);
    void renderFailure(QString reason);

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
    void releaseResources() override;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

// This experiment branch runs the onscreen benchmark by default. The regular
// application remains available through --regular-app.
int runTrickplayBenchmark(int argc, char **argv);

} // namespace Spool
