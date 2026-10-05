#include "TrickplayPreviewItem.h"
#include "TrickplayService.h"

#include <QLoggingCategory>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QVector4D>
#include <algorithm>
#include <cstring>
#include <functional>
#include <rhi/qrhi.h>

Q_LOGGING_CATEGORY(trickplayRender, "spool.trickplay.render", QtWarningMsg)

namespace Spool {
namespace {
    class PlaneTexture final : public QSGTexture {
    public:
        PlaneTexture(QRhi *rhi, QRhiTexture::Format format, const QSize& size)
            : texture(rhi->newTexture(format, size))
        {
            setFiltering(Linear);
            setMipmapFiltering(None);
            setHorizontalWrapMode(ClampToEdge);
            setVerticalWrapMode(ClampToEdge);
        }
        qint64 comparisonKey() const override
        {
            return qint64(quintptr(texture.get()));
        }
        QRhiTexture *rhiTexture() const override
        {
            return texture.get();
        }
        QSize textureSize() const override
        {
            return texture->pixelSize();
        }
        bool hasAlphaChannel() const override
        {
            return false;
        }
        bool hasMipmaps() const override
        {
            return false;
        }
        void stage(QByteArray data, int stride)
        {
            pending = std::move(data);
            rowStride = stride;
        }
        void stageImage(const QImage& image)
        {
            pendingImage = image;
        }
        void commitTextureOperations(QRhi *, QRhiResourceUpdateBatch *batch) override
        {
            if ((pending.isEmpty() && pendingImage.isNull()) || !batch)
                return;
            QRhiTextureSubresourceUploadDescription upload = pendingImage.isNull()
                ? QRhiTextureSubresourceUploadDescription(pending)
                : QRhiTextureSubresourceUploadDescription(pendingImage);
            if (pendingImage.isNull())
                upload.setDataStride(quint32(rowStride));
            upload.setSourceSize(textureSize());
            batch->uploadTexture(texture.get(), QRhiTextureUploadDescription { QRhiTextureUploadEntry(0, 0, upload) });
            qCDebug(trickplayRender) << "upload" << textureSize() << "stride" << rowStride;
            pending.clear();
            pendingImage = {};
        }
        std::unique_ptr<QRhiTexture> texture;
        QByteArray pending;
        QImage pendingImage;
        int rowStride = 0;
    };

    class Material;
    class Shader final : public QSGMaterialShader {
    public:
        explicit Shader(bool planar)
        {
            setShaderFileName(VertexStage, QStringLiteral(":/trickplay/shaders/preview.vert.qsb"));
            setShaderFileName(FragmentStage,
                planar ? QStringLiteral(":/trickplay/shaders/yuv.frag.qsb")
                       : QStringLiteral(":/trickplay/shaders/rgb.frag.qsb"));
        }
        bool updateUniformData(RenderState&, QSGMaterial *, QSGMaterial *) override;
        void updateSampledImage(RenderState&, int, QSGTexture **, QSGMaterial *, QSGMaterial *) override;
    };
    class Material final : public QSGMaterial {
    public:
        explicit Material(bool planar)
            : yuv(planar)
        {
            setFlag(Blending);
        }
        QSGMaterialType *type() const override
        {
            static QSGMaterialType rgbType, yuvType;
            return yuv ? &yuvType : &rgbType;
        }
        QSGMaterialShader *createShader(QSGRendererInterface::RenderMode) const override
        {
            return new Shader(yuv);
        }
        int compare(const QSGMaterial *other) const override
        {
            return this == other ? 0 : (std::less<const QSGMaterial *>()(this, other) ? -1 : 1);
        }
        bool yuv;
        std::array<std::unique_ptr<PlaneTexture>, 3> textures;
        std::shared_ptr<const TrickplayTexture> frame;
        QImage fallback;
        // options: alpha plane selector, BGRA swizzle; dimensions are actual JPEG,
        // not metadata/display dimensions. All per-sheet constants are CPU uniforms.
        std::array<QVector4D, 4> uniforms;
    };
    bool Shader::updateUniformData(RenderState& state, QSGMaterial *value, QSGMaterial *old)
    {
        auto *material = static_cast<Material *>(value);
        QByteArray *buffer = state.uniformData();
        if (state.isMatrixDirty() || !old)
            std::memcpy(buffer->data(), state.combinedMatrix().constData(), 64);
        if (state.isOpacityDirty() || !old) {
            const float opacity = state.opacity();
            std::memcpy(buffer->data() + 64, &opacity, sizeof(opacity));
        }
        for (size_t i = 0; i < material->uniforms.size(); ++i) {
            const auto& v = material->uniforms[i];
            const float data[] { v.x(), v.y(), v.z(), v.w() };
            std::memcpy(buffer->data() + 80 + 16 * i, data, 16);
        }
        return true;
    }
    void Shader::updateSampledImage(
        RenderState& state, int binding, QSGTexture **texture, QSGMaterial *value, QSGMaterial *)
    {
        auto *material = static_cast<Material *>(value);
        const int index = binding - 1;
        if (index < 0 || index >= (material->yuv ? 3 : 1))
            return;
        auto *plane = material->textures[size_t(index)].get();
        plane->commitTextureOperations(state.rhi(), state.resourceUpdateBatch());
        *texture = plane;
    }
    class PreviewNode final : public QSGGeometryNode {
    public:
        PreviewNode(bool planar, QRhi *backend, QQuickWindow *ownerWindow)
            : rhi(backend)
            , window(ownerWindow)
            , geometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4)
            , material(planar)
        {
            geometry.setDrawingMode(QSGGeometry::DrawTriangleStrip);
            geometry.setVertexDataPattern(QSGGeometry::DynamicPattern);
            setGeometry(&geometry);
            setMaterial(&material);
        }
        bool configure(QRhi *rhi, const std::shared_ptr<const TrickplayTexture>& frame, const QRectF& crop,
            const QRectF& destination)
        {
            const bool newFrame = material.frame != frame;
            auto format = material.yuv
                ? (rhi->isTextureFormatSupported(QRhiTexture::R8) ? QRhiTexture::R8 : QRhiTexture::RED_OR_ALPHA8)
                : (rhi->isTextureFormatSupported(QRhiTexture::BGRA8) ? QRhiTexture::BGRA8 : QRhiTexture::RGBA8);
            if (!rhi->isTextureFormatSupported(format))
                return false;
            const int limit = rhi->resourceLimit(QRhi::TextureSizeMax);
            if (frame->size.width() > limit || frame->size.height() > limit)
                return false;
            if (newFrame && !material.yuv)
                material.fallback = frame->image();
            const QSize chroma((frame->size.width() + 1) / 2, (frame->size.height() + 1) / 2);
            const int count = material.yuv ? 3 : 1;
            for (int i = 0; i < count; ++i) {
                const QSize size = i == 0 ? frame->size : chroma;
                auto& texture = material.textures[size_t(i)];
                const bool create
                    = !texture || texture->textureSize() != size || texture->rhiTexture()->format() != format;
                if (create) {
                    texture = std::make_unique<PlaneTexture>(rhi, format, size);
                    if (!texture->texture->create())
                        return false;
                    qCDebug(trickplayRender)
                        << (material.yuv ? "planar Lanczos2" : "RGB Lanczos2") << "texture format" << format << size;
                }
                if (newFrame || create) {
                    if (material.yuv)
                        texture->stage(frame->planes[size_t(i)], frame->strides[size_t(i)]);
                    else
                        texture->stageImage(material.fallback);
                }
            }
            material.frame = frame;
            const float w = frame->size.width(), h = frame->size.height();
            const float cw = chroma.width(), ch = chroma.height();
            material.uniforms[0] = QVector4D(
                material.yuv && format == QRhiTexture::RED_OR_ALPHA8 && !rhi->isFeatureSupported(QRhi::RedOrAlpha8IsRed)
                    ? 1.f
                    : 0.f,
                !material.yuv && format == QRhiTexture::RGBA8 ? 1.f : 0.f, 0.f, 0.f);
            material.uniforms[1] = QVector4D(w, h, 1.f / w, 1.f / h);
            material.uniforms[2]
                = QVector4D(crop.left() + .5f, crop.top() + .5f, crop.right() - .5f, crop.bottom() - .5f);
            material.uniforms[3] = QVector4D(1.f / cw, 1.f / ch, .5f / cw, .5f / ch);
            QSGGeometry::updateTexturedRectGeometry(
                &geometry, destination, QRectF(crop.x() / w, crop.y() / h, crop.width() / w, crop.height() / h));
            geometry.markVertexDataDirty();
            markDirty(DirtyGeometry | DirtyMaterial);
            return true;
        }
        QRhi *rhi;
        QQuickWindow *window;
        QSGGeometry geometry;
        Material material;
    };
    class SoftwareNode final : public QSGSimpleTextureNode {
    public:
        std::shared_ptr<const TrickplayTexture> frame;
        QImage image;
        QRhi *rhi = nullptr;
        QQuickWindow *window = nullptr;
    };
} // namespace

TrickplayPreviewItem::TrickplayPreviewItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents);
}
TrickplayPreviewItem::~TrickplayPreviewItem()
{
    if (m_response) {
        disconnect(m_response, nullptr, this, nullptr);
        connect(m_response, &QQuickImageResponse::finished, m_response, &QObject::deleteLater);
        m_response->cancel();
    }
}
void TrickplayPreviewItem::setSource(const QUrl& source)
{
    if (m_source == source)
        return;
    if (m_response) {
        disconnect(m_response, nullptr, this, nullptr);
        connect(m_response, &QQuickImageResponse::finished, m_response, &QObject::deleteLater);
        m_response->cancel();
        m_response.clear();
    }
    m_source = source;
    m_frame.reset();
    emit sourceChanged();
    emit readyChanged();
    update();
    auto *engine = qmlEngine(this);
    auto *provider = engine && source.scheme() == QLatin1String("image")
        ? dynamic_cast<QQuickAsyncImageProvider *>(engine->imageProvider(source.host()))
        : nullptr;
    if (!provider)
        return;
    QString id = source.path();
    if (id.startsWith('/'))
        id.remove(0, 1);
    QQuickImageResponse *response = provider->requestImageResponse(id, {});
    if (!response)
        return;
    m_response = response;
    connect(response, &QQuickImageResponse::finished, this, [this, response] {
        if (m_response == response) {
            std::unique_ptr<QQuickTextureFactory> factory(response->textureFactory());
            if (auto *preview = dynamic_cast<TrickplayTextureFactory *>(factory.get()))
                m_frame = preview->texture;
            else if (!response->errorString().isEmpty())
                qCDebug(trickplayRender) << response->errorString();
            m_response.clear();
            emit readyChanged();
            update();
        }
        response->deleteLater();
    });
}
void TrickplayPreviewItem::setCrop(const QRectF& crop)
{
    if (m_crop == crop)
        return;
    m_crop = crop;
    emit cropChanged();
    update();
}
QSGNode *TrickplayPreviewItem::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    if (!m_frame || width() <= 0 || height() <= 0) {
        delete old;
        return nullptr;
    }
    const QRectF full(QPointF(), QSizeF(m_frame->size));
    const QRectF crop = m_crop.isValid() ? m_crop.intersected(full) : full;
    if (crop.isEmpty()) {
        delete old;
        return nullptr;
    }
    auto *rhi
        = static_cast<QRhi *>(window()->rendererInterface()->getResource(window(), QSGRendererInterface::RhiResource));
    auto *fallback = dynamic_cast<SoftwareNode *>(old);
    const bool retainFallback
        = fallback && fallback->frame == m_frame && fallback->rhi == rhi && fallback->window == window();
    // A rejected frame stays on its cached RGB texture until either the decoded
    // frame or rendering context changes. Crop/geometry updates need no retry.
    if (rhi && !retainFallback) {
        const bool planar = m_frame->planar()
            && (rhi->isTextureFormatSupported(QRhiTexture::R8)
                || rhi->isTextureFormatSupported(QRhiTexture::RED_OR_ALPHA8));
        auto *node = dynamic_cast<PreviewNode *>(old);
        if (!node || node->material.yuv != planar || node->rhi != rhi || node->window != window()) {
            delete old;
            node = new PreviewNode(planar, rhi, window());
        }
        if (node->configure(rhi, m_frame, crop, boundingRect()))
            return node;
        delete node;
        old = nullptr;
    }
    // Software/unsupported RHI paths retain a single RGB texture, not a CPU
    // crop or a second resampling pass. Their quality is the backend's filter.
    auto *node = dynamic_cast<SoftwareNode *>(old);
    if (!node || node->rhi != rhi || node->window != window()) {
        delete old;
        node = new SoftwareNode;
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
        node->rhi = rhi;
        node->window = window();
    }
    if (node->frame != m_frame) {
        node->image = m_frame->image();
        QSGTexture *texture = window()->createTextureFromImage(node->image);
        if (!texture) {
            delete node;
            return nullptr;
        }
        QSGTexture *previous = node->texture();
        node->setOwnsTexture(false);
        node->setTexture(texture);
        node->setOwnsTexture(true);
        delete previous;
        node->frame = m_frame;
        qCDebug(trickplayRender) << "software/backend RGB fallback texture create" << m_frame->size;
    }
    node->setRect(boundingRect());
    // The backend may downscale an oversized atlas. Source rectangles are in
    // texture pixels, whereas crop coordinates refer to the decoded atlas.
    const QSize textureSize = node->texture()->textureSize();
    const qreal scaleX = qreal(textureSize.width()) / m_frame->size.width();
    const qreal scaleY = qreal(textureSize.height()) / m_frame->size.height();
    node->setSourceRect(QRectF(crop.x() * scaleX, crop.y() * scaleY, crop.width() * scaleX, crop.height() * scaleY));
    return node;
}
} // namespace Spool
