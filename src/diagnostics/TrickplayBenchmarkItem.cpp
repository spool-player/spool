#include "TrickplayBenchmark.h"

#include <QElapsedTimer>
#include <QFile>
#include <QQuickGraphicsConfiguration>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTexture>
#include <QVector4D>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <rhi/qrhi.h>
#include <rhi/qshader.h>

namespace Spool {
namespace {

    // Only the render thread accesses these counters. Nodes retain them independently
    // of the GUI item, so scene graph teardown never dereferences the item's State.
    struct RenderRecord {
        quint64 serial = 0;
        bool pending = false;
        bool uploaded = false;
        bool expectsUpload = false;
        bool sampled = false;
        bool blank = true;
        qint64 itemSyncNs = 0;
        qint64 syncNs = 0;
        qint64 textureCreateNs = 0;
        qint64 materialUpdateNs = 0;
        std::array<qint64, 3> uploadNs {};
        quint64 textureBytes = 0;
        quint64 sourceBytes = 0;
        int textureCount = 0;
        QString textureFormat;
        QString error;
        QElapsedTimer syncTimer;
        QElapsedTimer renderTimer;
        // Qt 6.11 Vulkan has exactly two frame-resource slots. These are
        // non-owning context identities only; invalidation clears all history.
        QRhi *gpuHistoryRhi = nullptr;
        QRhiSwapChain *gpuHistorySwapChain = nullptr;
        std::array<quint64, 2> gpuSlotSerial {};
    };

    class PlaneTexture final : public QSGTexture {
    public:
        PlaneTexture(QRhi *rhi, QRhiTexture::Format format, const QSize& size, int plane,
            const std::shared_ptr<RenderRecord>& record)
            : m_texture(rhi->newTexture(format, size))
            , m_plane(plane)
            , m_record(record)
        {
            setFiltering(Linear);
            setMipmapFiltering(None);
            setHorizontalWrapMode(ClampToEdge);
            setVerticalWrapMode(ClampToEdge);
        }

        bool create()
        {
            return m_texture && m_texture->create();
        }
        qint64 comparisonKey() const override
        {
            return qint64(quintptr(m_texture.get()));
        }
        QRhiTexture *rhiTexture() const override
        {
            return m_texture.get();
        }
        QSize textureSize() const override
        {
            return m_texture->pixelSize();
        }
        bool hasAlphaChannel() const override
        {
            return false;
        }
        bool hasMipmaps() const override
        {
            return false;
        }

        void stage(const QByteArray& data, int stride)
        {
            // QByteArray's implicit sharing avoids a full decoded-plane copy. The
            // node retains the frame as well, including a Qt QImage fromRawData owner.
            m_data = data;
            m_stride = stride;
            m_pending = true;
        }

        void commitTextureOperations(QRhi *, QRhiResourceUpdateBatch *batch) override
        {
            if (!m_pending)
                return;
            if (!batch) {
                m_record->error = QStringLiteral("No scene graph resource update batch for texture upload");
                return;
            }
            QElapsedTimer timer;
            timer.start();
            QRhiTextureSubresourceUploadDescription subresource(m_data);
            subresource.setDataStride(quint32(m_stride));
            subresource.setSourceSize(textureSize());
            batch->uploadTexture(
                m_texture.get(), QRhiTextureUploadDescription { QRhiTextureUploadEntry(0, 0, subresource) });
            m_pending = false;
            m_data.clear();
            m_record->uploadNs[size_t(m_plane)] += timer.nsecsElapsed();
            m_record->uploaded = true;
        }

    private:
        std::unique_ptr<QRhiTexture> m_texture;
        int m_plane;
        std::shared_ptr<RenderRecord> m_record;
        QByteArray m_data;
        int m_stride = 0;
        bool m_pending = false;
    };

    class PreviewMaterial;
    class PreviewShader final : public QSGMaterialShader {
    public:
        explicit PreviewShader(bool yuv)
        {
            setShaderFileName(VertexStage, QStringLiteral(":/bench/shaders/preview.vert.qsb"));
            setShaderFileName(FragmentStage,
                yuv ? QStringLiteral(":/bench/shaders/yuv.frag.qsb") : QStringLiteral(":/bench/shaders/rgb.frag.qsb"));
        }
        bool updateUniformData(RenderState& state, QSGMaterial *newMaterial, QSGMaterial *oldMaterial) override;
        void updateSampledImage(
            RenderState& state, int binding, QSGTexture **texture, QSGMaterial *newMaterial, QSGMaterial *) override;
    };

    class PreviewMaterial final : public QSGMaterial {
    public:
        PreviewMaterial(bool planar, std::shared_ptr<RenderRecord> metrics)
            : yuv(planar)
            , record(std::move(metrics))
        {
            // The benchmark draws an opaque JPEG at item/window opacity 1.
            // Neither path needs a destination blend read in this experiment.
        }
        QSGMaterialType *type() const override
        {
            static QSGMaterialType rgbType;
            static QSGMaterialType yuvType;
            return yuv ? &yuvType : &rgbType;
        }
        QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const override
        {
            return new PreviewShader(yuv);
        }
        int compare(const QSGMaterial *other) const override
        {
            // Each benchmark window has one quad; prohibit accidental batching with
            // different resources/crops if another preview is ever present.
            if (this == other)
                return 0;
            return std::less<const QSGMaterial *>()(this, other) ? -1 : 1;
        }
        bool yuv;
        std::shared_ptr<RenderRecord> record;
        std::array<std::unique_ptr<PlaneTexture>, 3> textures;
        QVector4D options;
        QVector4D yBounds;
        QVector4D chromaBounds;
        QVector4D chromaScale;
    };

    bool PreviewShader::updateUniformData(RenderState& state, QSGMaterial *newMaterial, QSGMaterial *oldMaterial)
    {
        auto *material = static_cast<PreviewMaterial *>(newMaterial);
        QElapsedTimer timer;
        timer.start();
        QByteArray *buffer = state.uniformData();
        if (state.isMatrixDirty() || !oldMaterial) {
            const QMatrix4x4 matrix = state.combinedMatrix();
            std::memcpy(buffer->data(), matrix.constData(), 64);
        }
        if (state.isOpacityDirty() || !oldMaterial) {
            const float opacity = state.opacity();
            std::memcpy(buffer->data() + 64, &opacity, sizeof(opacity));
        }
        // std140 puts the next vec4 at byte 80, not immediately after opacity.
        // A material can be reused in-place for crop updates, so oldMaterial pointer
        // equality must not be used to skip these small uniform writes.
        const std::array<QVector4D, 4> values { material->options, material->yBounds, material->chromaBounds,
            material->chromaScale };
        for (size_t i = 0; i < values.size(); ++i) {
            const std::array<float, 4> value { values[i].x(), values[i].y(), values[i].z(), values[i].w() };
            std::memcpy(buffer->data() + 80 + 16 * i, value.data(), 16);
        }
        material->record->materialUpdateNs += timer.nsecsElapsed();
        return true;
    }

    void PreviewShader::updateSampledImage(
        RenderState& state, int binding, QSGTexture **texture, QSGMaterial *newMaterial, QSGMaterial *)
    {
        auto *material = static_cast<PreviewMaterial *>(newMaterial);
        const int index = binding - 1;
        if (index < 0 || index >= (material->yuv ? 3 : 1))
            return;
        auto *plane = material->textures[size_t(index)].get();
        plane->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
        QElapsedTimer timer;
        timer.start();
        *texture = plane;
        material->record->sampled = true;
        material->record->materialUpdateNs += timer.nsecsElapsed();
    }

    class PreviewNode final : public QSGGeometryNode {
    public:
        PreviewNode(bool yuv, const std::shared_ptr<RenderRecord>& record)
            : m_geometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4)
            , m_material(yuv, record)
        {
            m_geometry.setDrawingMode(QSGGeometry::DrawTriangleStrip);
            m_geometry.setVertexDataPattern(QSGGeometry::DynamicPattern);
            setGeometry(&m_geometry);
            setMaterial(&m_material);
        }

        bool configure(
            QRhi *rhi, std::shared_ptr<const TrickplayBenchFrame> frame, const QRect& crop, const QRectF& destination)
        {
            const auto& record = m_material.record;
            const bool yuv = frame->format == TrickplayBenchFormat::Yuv420;
            const int count = yuv ? 3 : 1;
            const QSize chroma((frame->size.width() + 1) / 2, (frame->size.height() + 1) / 2);
            const bool newFrame = frame.get() != m_frame.get();
            QRhiTexture::Format format = QRhiTexture::BGRA8;
            if (yuv) {
                format = rhi->isTextureFormatSupported(QRhiTexture::R8) ? QRhiTexture::R8 : QRhiTexture::RED_OR_ALPHA8;
            } else if (!rhi->isTextureFormatSupported(format)) {
                format = QRhiTexture::RGBA8;
            }
            if (!rhi->isTextureFormatSupported(format)) {
                record->error = QStringLiteral("Backend cannot sample the required compact texture format");
                return false;
            }
            m_material.options = QVector4D(
                yuv && format == QRhiTexture::RED_OR_ALPHA8 && !rhi->isFeatureSupported(QRhi::RedOrAlpha8IsRed) ? 1.f
                                                                                                                : 0.f,
                !yuv && format == QRhiTexture::RGBA8 ? 1.f : 0.f, 0.f, 0.f);
            record->textureFormat = yuv
                ? (format == QRhiTexture::R8 ? QStringLiteral("R8")
                                             : (m_material.options.x() ? QStringLiteral("RED_OR_ALPHA8(alpha)")
                                                                       : QStringLiteral("RED_OR_ALPHA8(red)")))
                : (format == QRhiTexture::BGRA8 ? QStringLiteral("BGRA8")
                                                : QStringLiteral("RGBA8+shader-BGRA-swizzle"));
            record->textureBytes = 0;
            record->sourceBytes = 0;
            record->textureCount = count;
            const int limit = rhi->resourceLimit(QRhi::TextureSizeMax);
            for (int i = 0; i < count; ++i) {
                const QSize size = i == 0 ? frame->size : chroma;
                const int bytesPerPixel = yuv ? 1 : 4;
                const qint64 rowBytes = qint64(size.width()) * bytesPerPixel;
                const qint64 requiredBytes = qint64(size.height() - 1) * frame->strides[size_t(i)] + rowBytes;
                if (size.isEmpty() || size.width() > limit || size.height() > limit) {
                    record->error = QStringLiteral("Texture %1 size %2x%3 exceeds backend maximum %4")
                                        .arg(i)
                                        .arg(size.width())
                                        .arg(size.height())
                                        .arg(limit);
                    return false;
                }
                if (frame->strides[size_t(i)] < rowBytes || frame->planes[size_t(i)].size() < requiredBytes) {
                    record->error
                        = QStringLiteral("Decoded plane %1 has invalid stride or insufficient visible rows").arg(i);
                    return false;
                }
                auto& texture = m_material.textures[size_t(i)];
                const bool recreate
                    = !texture || texture->textureSize() != size || texture->rhiTexture()->format() != format;
                if (recreate) {
                    QElapsedTimer timer;
                    timer.start();
                    texture = std::make_unique<PlaneTexture>(rhi, format, size, i, record);
                    const bool created = texture->create();
                    record->textureCreateNs += timer.nsecsElapsed();
                    if (!created) {
                        record->error = QStringLiteral("Creating GPU texture %1 failed").arg(i);
                        return false;
                    }
                }
                if (newFrame || recreate) {
                    texture->stage(frame->planes[size_t(i)], frame->strides[size_t(i)]);
                    record->expectsUpload = true;
                }
                record->textureBytes += quint64(size.width()) * quint64(size.height()) * quint64(bytesPerPixel);
                record->sourceBytes += quint64(frame->planes[size_t(i)].size());
            }
            m_frame = std::move(frame);
            QElapsedTimer timer;
            timer.start();
            const float w = float(m_frame->size.width());
            const float h = float(m_frame->size.height());
            const float cw = float(chroma.width());
            const float ch = float(chroma.height());
            const int right = crop.x() + crop.width();
            const int bottom = crop.y() + crop.height();
            m_material.yBounds = QVector4D((float(crop.x()) + .5f) / w, (float(crop.y()) + .5f) / h,
                (float(right) - .5f) / w, (float(bottom) - .5f) / h);
            // RGB was reconstructed from the whole JPEG, including chroma
            // neighbours across thumbnail borders. Match that reconstruction.
            m_material.chromaBounds = QVector4D(.5f / cw, .5f / ch, (cw - .5f) / cw, (ch - .5f) / ch);
            m_material.chromaScale = QVector4D(w / (2.f * cw), h / (2.f * ch), 0.f, 0.f);
            QSGGeometry::updateTexturedRectGeometry(&m_geometry, destination,
                QRectF(float(crop.x()) / w, float(crop.y()) / h, float(crop.width()) / w, float(crop.height()) / h));
            m_geometry.markVertexDataDirty();
            markDirty(DirtyGeometry | DirtyMaterial);
            record->materialUpdateNs += timer.nsecsElapsed();
            return true;
        }
        bool isYuv() const
        {
            return m_material.yuv;
        }

    private:
        QSGGeometry m_geometry;
        PreviewMaterial m_material;
        std::shared_ptr<const TrickplayBenchFrame> m_frame;
    };

    bool shadersAvailable(QString *error)
    {
        for (const char *name :
            { ":/bench/shaders/preview.vert.qsb", ":/bench/shaders/rgb.frag.qsb", ":/bench/shaders/yuv.frag.qsb" }) {
            QFile file(QString::fromLatin1(name));
            if (!file.open(QIODevice::ReadOnly)) {
                *error = QStringLiteral("Missing benchmark shader: %1").arg(QString::fromLatin1(name));
                return false;
            }
            const QShader shader = QShader::fromSerialized(file.readAll());
            if (!shader.isValid()
                || (shader.stage() == QShader::VertexStage
                    && shader
                        .shader(QShaderKey(QShader::SpirvShader, QShaderVersion(100), QShader::BatchableVertexShader))
                        .shader()
                        .isEmpty())) {
                *error = QStringLiteral("Invalid benchmark shader or missing scene-graph batchable variant: %1")
                             .arg(QString::fromLatin1(name));
                return false;
            }
        }
        return true;
    }

} // namespace

struct TrickplayBenchmarkItem::State {
    std::shared_ptr<const TrickplayBenchFrame> frame;
    QRect crop;
    quint64 serial = 0;
    bool requested = false;
    bool checkedShaders = false;
    QString shaderError;
    std::shared_ptr<RenderRecord> record = std::make_shared<RenderRecord>();
    QList<QMetaObject::Connection> connections;
};

TrickplayBenchmarkItem::TrickplayBenchmarkItem(QQuickItem *parent)
    : QQuickItem(parent)
    , m_state(std::make_unique<State>())
{
    setFlag(ItemHasContents);
    const auto attachWindow = [this](QQuickWindow *window) {
        for (const auto& connection : m_state->connections)
            disconnect(connection);
        m_state->connections.clear();
        if (!window)
            return;
        const auto record = m_state->record;
        auto& connections = m_state->connections;
        connections.append(connect(
            window, &QQuickWindow::beforeSynchronizing, this, [record] { record->syncTimer.start(); },
            Qt::DirectConnection));
        connections.append(connect(
            window, &QQuickWindow::afterSynchronizing, this,
            [record] { record->syncNs = record->syncTimer.nsecsElapsed(); }, Qt::DirectConnection));
        connections.append(connect(
            window, &QQuickWindow::beforeRendering, this, [record] { record->renderTimer.start(); },
            Qt::DirectConnection));
        connections.append(connect(
            window, &QQuickWindow::afterRendering, this,
            [this, record, window] {
                const qint64 renderNs = record->renderTimer.isValid() ? record->renderTimer.nsecsElapsed() : 0;
                const qint64 syncRenderNs = record->syncTimer.isValid() ? record->syncTimer.nsecsElapsed() : 0;
                QRhi *rhi = window->rhi();
                if (!rhi) {
                    if (record->pending) {
                        record->pending = false;
                        emit renderFailure(QStringLiteral("Qt scene graph has no RHI backend"));
                    }
                    return;
                }
                const bool timestampSupport = rhi->isFeatureSupported(QRhi::Timestamps);
                const bool timestampsEnabled = window->graphicsConfiguration().timestampsEnabled();
                const bool originAttribution = rhi->backend() == QRhi::Vulkan;
                QRhiSwapChain *swapChain = window->swapChain();
                double completedSeconds = 0;
                if (timestampSupport && timestampsEnabled && swapChain) {
                    if (QRhiCommandBuffer *buffer = swapChain->currentFrameCommandBuffer())
                        completedSeconds = buffer->lastCompletedGpuTime();
                }
                const bool completedTimeAvailable = std::isfinite(completedSeconds) && completedSeconds > 0;
                QVariant delayedGpuMs;
                if (completedTimeAvailable)
                    delayedGpuMs = completedSeconds * 1000.0;
                if (record->gpuHistoryRhi != rhi || record->gpuHistorySwapChain != swapChain) {
                    record->gpuSlotSerial = {};
                    record->gpuHistoryRhi = rhi;
                    record->gpuHistorySwapChain = swapChain;
                }
                if (originAttribution && swapChain) {
                    // Verified in Qt 6.11 qrhivulkan.cpp beginFrame(): the
                    // completed query belongs to the previous use of THIS
                    // frame-resource slot, not simply the previous Qt frame.
                    // prepareNewFrame()/QVkCommandBuffer::resetState clears
                    // lastGpuTime to zero before a fresh result is assigned.
                    const int slot = rhi->currentFrameSlot();
                    if (slot >= 0 && size_t(slot) < record->gpuSlotSerial.size()) {
                        quint64& previousSerial = record->gpuSlotSerial[size_t(slot)];
                        if (completedTimeAvailable && previousSerial != 0) {
                            emit gpuSampleReady(
                                QVariantMap { { QStringLiteral("serial"), QVariant::fromValue(previousSerial) },
                                    { QStringLiteral("gpu_frame_ms"), completedSeconds * 1000.0 },
                                    { QStringLiteral("backend"), QString::fromLatin1(rhi->backendName()) },
                                    { QStringLiteral("gpu_frame_slot"), slot },
                                    { QStringLiteral("scope"),
                                        QStringLiteral("whole Qt frame including uploads/draw; Qt 6.11 Vulkan previous "
                                                       "use of same frame slot; not isolated pass") } });
                        }
                        // Track every actual frame, including caption-only and
                        // blank frames. Otherwise an intervening unrequested
                        // render would incorrectly inherit a benchmark serial.
                        previousSerial = record->pending && !record->blank ? record->serial : 0;
                    }
                }
                if (!record->pending)
                    return;
                record->pending = false;
                if (!record->error.isEmpty()) {
                    emit renderFailure(record->error);
                    return;
                }
                if (!record->blank && (!record->sampled || (record->expectsUpload && !record->uploaded))) {
                    emit renderFailure(
                        QStringLiteral("Preview material was not prepared/uploaded in the requested frame"));
                    return;
                }
                // Statistics are read after the CPU timing endpoints. These are
                // whole-QRhi allocator figures, not isolated texture or physical
                // VRAM residency measurements. Other backends' zero placeholders
                // must remain unavailable rather than appearing to measure zero.
                const QRhiStats stats = rhi->statistics();
                const bool allocatorStats = rhi->backend() == QRhi::Vulkan || rhi->backend() == QRhi::D3D12;
                const bool d3d12Stats = rhi->backend() == QRhi::D3D12;
                QVariantMap sample { { QStringLiteral("serial"), QVariant::fromValue(record->serial) },
                    { QStringLiteral("sync_ns"), record->syncNs },
                    { QStringLiteral("item_sync_ns"), record->itemSyncNs },
                    { QStringLiteral("texture_create_ns"), record->textureCreateNs },
                    { QStringLiteral("upload_enqueue_ns"),
                        record->uploadNs[0] + record->uploadNs[1] + record->uploadNs[2] },
                    { QStringLiteral("upload_y_ns"), record->uploadNs[0] },
                    { QStringLiteral("upload_u_ns"), record->uploadNs[1] },
                    { QStringLiteral("upload_v_ns"), record->uploadNs[2] },
                    { QStringLiteral("material_update_ns"), record->materialUpdateNs },
                    { QStringLiteral("render_cpu_ns"), renderNs },
                    { QStringLiteral("sync_and_render_cpu_ns"), syncRenderNs },
                    { QStringLiteral("gpu_texture_bytes"), QVariant::fromValue(record->textureBytes) },
                    { QStringLiteral("retained_source_bytes"), QVariant::fromValue(record->sourceBytes) },
                    { QStringLiteral("texture_count"), record->textureCount },
                    { QStringLiteral("texture_format"), record->textureFormat },
                    { QStringLiteral("uploaded"), record->uploaded }, { QStringLiteral("blank"), record->blank },
                    { QStringLiteral("backend"), QString::fromLatin1(rhi->backendName()) },
                    { QStringLiteral("device"), QString::fromUtf8(rhi->driverInfo().deviceName) },
                    { QStringLiteral("rhi_allocator_stats_supported"), allocatorStats },
                    { QStringLiteral("rhi_allocator_used_bytes"),
                        allocatorStats ? QVariant::fromValue(stats.usedBytes) : QVariant() },
                    { QStringLiteral("rhi_allocator_unused_bytes"),
                        allocatorStats ? QVariant::fromValue(stats.unusedBytes) : QVariant() },
                    { QStringLiteral("rhi_allocator_allocated_bytes"),
                        allocatorStats ? QVariant::fromValue(quint64(stats.usedBytes) + quint64(stats.unusedBytes))
                                       : QVariant() },
                    { QStringLiteral("rhi_allocator_block_count"),
                        allocatorStats ? QVariant::fromValue(stats.blockCount) : QVariant() },
                    { QStringLiteral("rhi_allocator_allocation_count"),
                        allocatorStats ? QVariant::fromValue(stats.allocCount) : QVariant() },
                    { QStringLiteral("rhi_d3d12_total_usage_bytes"),
                        d3d12Stats ? QVariant::fromValue(stats.totalUsageBytes) : QVariant() },
                    { QStringLiteral("rhi_allocator_scope"),
                        QStringLiteral("whole QRhi Vulkan/D3D12 allocator; allocated=used+unused block bytes; includes "
                                       "Qt resources and deferred deletions; not physical VRAM residency; D3D12 "
                                       "total_usage is QueryVideoMemoryInfo") },
                    { QStringLiteral("rhi_total_pipeline_creation_ms"), stats.totalPipelineCreationTime },
                    { QStringLiteral("rhi_pipeline_creation_scope"),
                        QStringLiteral("cumulative whole QRhi graphics/compute pipeline creation time; setup "
                                       "diagnostic, backend-dependent") },
                    { QStringLiteral("gpu_timestamps_supported"), timestampSupport },
                    { QStringLiteral("gpu_timestamps_enabled"), timestampsEnabled },
                    { QStringLiteral("gpu_origin_attribution_supported"), originAttribution },
                    { QStringLiteral("last_completed_frame_gpu_ms"), delayedGpuMs },
                    { QStringLiteral("gpu_time_scope"),
                        QStringLiteral(
                            "delayed whole Qt frame; originating serial unknown; not isolated upload/draw") },
                    { QStringLiteral("upload_gpu_ms"), QVariant() }, { QStringLiteral("draw_gpu_ms"), QVariant() },
                    { QStringLiteral("gpu_texture_bytes_scope"),
                        QStringLiteral("logical visible texture allocation; backend may defer native deletion") },
                    { QStringLiteral("cpu_timing_scope"),
                        QStringLiteral("sync=whole Qt synchronization; render=beforeRendering to afterRendering "
                                       "command recording; upload=enqueue only") } };
                emit sampleReady(std::move(sample));
            },
            Qt::DirectConnection));
        connections.append(connect(
            window, &QQuickWindow::sceneGraphInvalidated, this,
            [this, record] {
                // Qt owns/deletes the geometry node and its QRhi resources on this
                // render thread. Do not keep any GPU pointers in the GUI State.
                if (record->pending)
                    emit renderFailure(QStringLiteral("Scene graph invalidated before the requested frame completed"));
                record->pending = false;
                record->gpuSlotSerial = {};
                record->gpuHistoryRhi = nullptr;
                record->gpuHistorySwapChain = nullptr;
            },
            Qt::DirectConnection));
        connections.append(connect(window, &QQuickWindow::sceneGraphError, this,
            [this](QQuickWindow::SceneGraphError, const QString& message) { emit renderFailure(message); }));
    };
    connect(this, &QQuickItem::windowChanged, this, attachWindow);
    // A parent supplied to QQuickItem's constructor already has a window before
    // this subclass can connect windowChanged.
    attachWindow(window());
}

TrickplayBenchmarkItem::~TrickplayBenchmarkItem()
{
    for (const auto& connection : m_state->connections)
        disconnect(connection);
}

void TrickplayBenchmarkItem::setFrame(
    std::shared_ptr<const TrickplayBenchFrame> frame, const QRect& crop, quint64 serial)
{
    m_state->frame = std::move(frame);
    m_state->crop = crop;
    m_state->serial = serial;
    m_state->requested = true;
    update();
}

void TrickplayBenchmarkItem::clearFrame(quint64 serial)
{
    setFrame({}, {}, serial);
}

QSGNode *TrickplayBenchmarkItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    QElapsedTimer timer;
    timer.start();
    const auto record = m_state->record;
    if (m_state->requested) {
        record->serial = m_state->serial;
        record->pending = true;
        record->uploaded = false;
        record->expectsUpload = false;
        record->sampled = false;
        record->textureCreateNs = 0;
        record->materialUpdateNs = 0;
        record->uploadNs = {};
        record->error.clear();
        m_state->requested = false;
    }
    record->blank = !m_state->frame;
    if (record->blank) {
        delete oldNode;
        record->textureBytes = 0;
        record->sourceBytes = 0;
        record->textureCount = 0;
        record->textureFormat.clear();
        record->itemSyncNs = timer.nsecsElapsed();
        return nullptr;
    }
    if (!m_state->checkedShaders) {
        shadersAvailable(&m_state->shaderError);
        m_state->checkedShaders = true;
    }
    QRhi *rhi = window() ? window()->rhi() : nullptr;
    if (!rhi || rhi->backend() == QRhi::Null || !m_state->shaderError.isEmpty()) {
        record->error = !m_state->shaderError.isEmpty()
            ? m_state->shaderError
            : QStringLiteral("Benchmark requires a real RHI scene graph, not the null/software scene graph");
        delete oldNode;
        record->itemSyncNs = timer.nsecsElapsed();
        return nullptr;
    }
#if Q_BYTE_ORDER != Q_LITTLE_ENDIAN
    if (m_state->frame->format == TrickplayBenchFormat::Rgb32) {
        record->error = QStringLiteral("RGB32 benchmark byte layout requires a little-endian target");
        delete oldNode;
        record->itemSyncNs = timer.nsecsElapsed();
        return nullptr;
    }
#endif
    const QRect visible(QPoint(0, 0), m_state->frame->size);
    if (m_state->crop.isEmpty() || !visible.contains(m_state->crop) || width() <= 0 || height() <= 0) {
        record->error = QStringLiteral("Invalid preview crop or destination dimensions");
        delete oldNode;
        record->itemSyncNs = timer.nsecsElapsed();
        return nullptr;
    }
    auto *node = static_cast<PreviewNode *>(oldNode);
    const bool yuv = m_state->frame->format == TrickplayBenchFormat::Yuv420;
    if (node && node->isYuv() != yuv) {
        delete node;
        node = nullptr;
    }
    if (!node)
        node = new PreviewNode(yuv, record);
    if (!node->configure(rhi, m_state->frame, m_state->crop, boundingRect())) {
        delete node;
        node = nullptr;
    }
    record->itemSyncNs = timer.nsecsElapsed();
    return node;
}

void TrickplayBenchmarkItem::releaseResources()
{
    // Called on the GUI thread. QQuickItem's scene graph lifecycle releases the
    // owned node on the render thread; never delete QRhi resources here.
}

} // namespace Spool
