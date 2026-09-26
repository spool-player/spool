#pragma once

#include <QElapsedTimer>
#include <QNetworkRequest>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <array>
#include <functional>

class QNetworkReply;

namespace Spool {

struct ScriptAccess;

// A worker-local benchmark. Destruction disconnects and aborts every lane;
// the owning ScriptOperation supplies the shared 15-second deadline.
class SpeedTest final : public QObject {
public:
    using Completion = std::function<void(QString error, qint64 bitrate, int parallelRequests)>;
    SpeedTest(ScriptAccess *access, Completion completion, QObject *parent);
    ~SpeedTest() override;
    void start(const QVariantMap& options);

private:
    struct Lane {
        QNetworkReply *reply = nullptr;
        qint64 received = 0;
        bool responseChecked = false;
    };
    void round(int lanes, qint64 totalBytes);
    bool drain(int lane);
    bool checkResponse(int lane);
    void finished(int lane);
    void finish(QString error = {}, qint64 bitrate = 0, int lanes = 1);
    void abort();

    ScriptAccess *m_access;
    Completion m_completion;
    QString m_template;
    QString m_nonce;
    QNetworkRequest m_request;
    std::array<Lane, 4> m_lanes;
    std::array<double, 3> m_rates {};
    std::array<char, 64 * 1024> m_buffer;
    QElapsedTimer m_elapsed;
    qint64 m_expected = 0;
    qint64 m_firstByteMs = -1;
    int m_count = 0;
    int m_remaining = 0;
    int m_round = -1;
    bool m_done = false;
    bool m_range = false;
};

} // namespace Spool
