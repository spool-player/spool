#pragma once

#include <QCoroTask>
#include <QUrl>
#include <QVariantMap>

namespace Spool {

struct DownloadRequest {
    QString itemId;
    bool transcode = false;
    qint64 maxBitrate = 0;
    int maxHeight = 0;
    QString variantId;
};

// A finite complete media file. Providers must not negotiate a live stream,
// HLS/DASH manifest, or any resource needing an online server for playback.
struct DownloadPlan {
    QUrl url;
    QString container;
    QVariantMap headers;
    qint64 size = -1;
    QVariantMap cleanup;
};

class DownloadSource {
public:
    virtual ~DownloadSource() = default;
    virtual QCoro::Task<DownloadPlan> negotiateDownload(DownloadRequest request, QString scope) = 0;
    virtual QCoro::Task<void> releaseDownload(QVariantMap cleanup) = 0;
};

} // namespace Spool
