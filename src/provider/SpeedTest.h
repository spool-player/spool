#pragma once

#include <QElapsedTimer>
#include <QNetworkRequest>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <array>
#include <functional>
#include <vector>

class QNetworkReply;

namespace Spool {

struct ScriptAccess;

// A worker-local benchmark. Destruction disconnects and aborts every lane;
// the owning ScriptOperation supplies the shared 15-second deadline.
class SpeedTest final : public QObject {
public:
    using Completion = std::function<void(QString error, QVariantMap result)>;
    SpeedTest(ScriptAccess *access, Completion completion, QObject *parent);
    ~SpeedTest() override;
    void start(const QVariantMap& options);

private:
    struct Lane {
        QNetworkReply *reply = nullptr;
        qint64 received = 0;
        bool responseChecked = false;
        qint64 firstByteNs = -1;
        int httpVersion = 0;
    };

    struct RoundSpec {
        int lanes = 1;
        bool forceHttp1 = false;
    };

    struct RoundSample {
        int requests = 1;
        int httpVersion = 0;
        double rateBps = 0.0;
        double ttfbMs = 0.0;
        double durationS = 0.0;
        qint64 receivedBytes = 0;
    };

    void round(int lanes, qint64 totalBytes, bool forceHttp1 = false);
    bool drain(int lane);
    bool checkResponse(int lane);
    void finished(int lane);
    void completeRound();
    void finish(QString error = {}, qint64 bitrate = 0);
    void abort();

    ScriptAccess *m_access;
    Completion m_completion;
    QString m_template;
    QString m_nonce;
    QNetworkRequest m_request;
    std::array<Lane, 8> m_lanes {};
    std::vector<RoundSpec> m_schedule;
    std::vector<RoundSample> m_samples;
    std::array<char, 64 * 1024> m_buffer;
    QElapsedTimer m_totalElapsed;
    QElapsedTimer m_elapsed;
    QElapsedTimer m_pointElapsed;
    QTimer m_roundDeadline;
    qint64 m_expected = 0;
    int m_count = 0;
    int m_remaining = 0;
    int m_round = -1;
    int m_transfer = 0;
    qint64 m_chunkBytes = 4 * 1024 * 1024;
    bool m_done = false;
    bool m_range = false;
    bool m_forceHttp1 = false;
    bool m_h3Advertised = false;
    bool m_addedWideRound = false;
    bool m_addedHttp1Round = false;
};

} // namespace Spool
