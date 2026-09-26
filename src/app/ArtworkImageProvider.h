#pragma once

#include "ArtworkService.h"

#include <QQuickImageProvider>

namespace Spool {

class ArtworkImageProvider final : public QQuickAsyncImageProvider {
public:
    explicit ArtworkImageProvider(ArtworkService *service);

    QQuickImageResponse *requestImageResponse(const QString& id, const QSize& requestedSize) override;

private:
    ArtworkService *m_service = nullptr;
};

} // namespace Spool
