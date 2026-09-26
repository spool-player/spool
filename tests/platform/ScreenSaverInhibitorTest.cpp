#include "platform/ScreenSaverInhibitor.h"

#include "TestMain.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace Spool {

std::unique_ptr<ScreenSaverBackend> createPlatformScreenSaverBackend()
{
    return {};
}

} // namespace Spool

namespace {

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

struct BackendState {
    int acquisitions = 0;
    int releases = 0;
    bool acquireSucceeds = true;
    bool releaseSucceeds = true;
};

class MockBackend final : public Spool::ScreenSaverBackend {
public:
    explicit MockBackend(BackendState& state)
        : m_state(state)
    {
    }

    bool acquire() override
    {
        ++m_state.acquisitions;
        return m_state.acquireSucceeds;
    }

    bool release() override
    {
        ++m_state.releases;
        return m_state.releaseSucceeds;
    }

private:
    BackendState& m_state;
};

} // namespace

SPOOL_TEST_MAIN("screensaver-inhibitor")
{
    using namespace Spool;

    require(screenSaverShouldBeInhibited(true, false), "playing video or audio should inhibit idle sleep");
    require(!screenSaverShouldBeInhibited(true, true), "paused playback should release idle inhibition");
    require(!screenSaverShouldBeInhibited(false, false), "stopped playback should release idle inhibition");
    require(screenSaverShouldBeInhibited(false, false, true),
        "an advancing slideshow should inhibit independently of media playback");

    BackendState state;
    {
        ScreenSaverInhibitor inhibitor(std::make_unique<MockBackend>(state));
        inhibitor.setInhibited(true);
        inhibitor.setInhibited(true);
        require(inhibitor.inhibited() && state.acquisitions == 1,
            "play and resume should acquire one idempotent platform lease");
        inhibitor.setInhibited(false);
        inhibitor.setInhibited(false);
        require(!inhibitor.inhibited() && state.releases == 1,
            "pause and stop should release one idempotent platform lease");
        inhibitor.setInhibited(true);
    }
    require(state.acquisitions == 2 && state.releases == 2,
        "destruction should release an outstanding platform cookie or execution-state lease");

    BackendState failure;
    failure.acquireSucceeds = false;
    ScreenSaverInhibitor failed(std::make_unique<MockBackend>(failure));
    failed.setInhibited(true);
    require(!failed.inhibited() && failure.acquisitions == 1 && failure.releases == 0,
        "a failed platform acquisition must not publish inhibited state");

    const QByteArray response
        = webOsScreenSaverResponsePayload(QByteArrayLiteral("{\"state\":\"Active\",\"timestamp\":12345}"));
    const QJsonObject responseObject = QJsonDocument::fromJson(response).object();
    require(responseObject.value(QStringLiteral("clientName")).toString() == QStringLiteral("com.sachk.spool")
            && !responseObject.value(QStringLiteral("ack")).toBool(true)
            && responseObject.value(QStringLiteral("timestamp")).toInt() == 12345,
        "webOS Active requests should receive the required ack:false response payload");
    require(webOsScreenSaverResponsePayload(QByteArrayLiteral("{\"state\":\"Inactive\"}")).isEmpty()
            && webOsScreenSaverResponsePayload(QByteArrayLiteral("not-json")).isEmpty(),
        "webOS should ignore inactive or malformed screensaver requests");

    return EXIT_SUCCESS;
}
