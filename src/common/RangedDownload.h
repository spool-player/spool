#pragma once

#include <QFile>
#include <QNetworkRequest>
#include <QObject>
#include <QPointer>

#include <functional>
#include <memory>
#include <vector>

class QNetworkAccessManager;
class QNetworkReply;

namespace Spool {

// One file fetched over several HTTP range requests at once, each on its own
// connection, written in place at its offsets. When a lane finishes it takes
// the back half of whichever lane has the most left, so every lane stays busy
// until the end rather than the slowest one finishing alone. A server that
// ignores ranges gets one ordinary GET.
//
// Integrity is the caller's: the file holds exactly the bytes the server sent.
class RangedDownload final : public QObject {
    Q_OBJECT

public:
    // Runs on every reply before it is read, e.g. to vet its redirects.
    using PrepareReply = std::function<void(QNetworkReply *)>;

    RangedDownload(QNetworkAccessManager *network, QNetworkRequest request, QString path, QObject *parent = nullptr);
    ~RangedDownload() override;

    // How many requests may run at once.
    void setLanes(int lanes);
    // The size the file must be; any other is an error. Unset, the server says.
    void setExpectedBytes(qint64 bytes);
    // A lane is never split into parts smaller than this.
    void setMinimumLaneBytes(qint64 bytes);
    void setPrepareReply(PrepareReply prepare);

    void start();
    void abort();
    qint64 receivedBytes() const;
    // -1 until the server has said.
    qint64 totalBytes() const;

signals:
    void progress();
    // Empty on success, when the file holds totalBytes() bytes.
    void finished(const QString& error);

private:
    struct Lane {
        QPointer<QNetworkReply> reply;
        qint64 next = 0;
        // Exclusive; -1 while the size is unknown.
        qint64 end = -1;
        bool checked = false;
        int retries = 0;
    };

    void request(Lane *lane);
    bool check(Lane *lane);
    bool drain(Lane *lane);
    void laneFinished(Lane *lane);
    void completeLane(Lane *lane);
    void rebalance();
    void restartWhole();
    void succeed();
    void fail(const QString& error);
    void stopLanes();
    bool setTotal(qint64 total);

    QNetworkAccessManager *m_network = nullptr;
    QNetworkRequest m_request;
    QFile m_file;
    PrepareReply m_prepare;
    std::vector<std::unique_ptr<Lane>> m_lanes;
    int m_laneLimit = 2;
    qint64 m_expected = -1;
    qint64 m_minimumLaneBytes = 1024 * 1024;
    qint64 m_total = -1;
    qint64 m_received = 0;
    bool m_ranged = true;
    bool m_running = false;
};

} // namespace Spool
