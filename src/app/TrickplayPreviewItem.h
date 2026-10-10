#pragma once

#include "TrickplayDecoder.h"
#include <QPointer>
#include <QQuickItem>
#include <QUrl>

class QQuickImageResponse;

namespace Spool {
class TrickplayPreviewItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QRectF crop READ crop WRITE setCrop NOTIFY cropChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
public:
    explicit TrickplayPreviewItem(QQuickItem *parent = nullptr);
    ~TrickplayPreviewItem() override;
    QUrl source() const
    {
        return m_source;
    }
    QRectF crop() const
    {
        return m_crop;
    }
    bool ready() const
    {
        return bool(m_frame);
    }
    void setSource(const QUrl& source);
    void setCrop(const QRectF& crop);
signals:
    void sourceChanged();
    void cropChanged();
    void readyChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;

private:
    QUrl m_source;
    QRectF m_crop;
    QPointer<QQuickImageResponse> m_response;
    std::shared_ptr<const TrickplayTexture> m_frame;
};
} // namespace Spool
